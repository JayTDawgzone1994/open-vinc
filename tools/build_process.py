"""Bounded build logs, directory locking and ownership of build subprocesses."""

import codecs
import contextlib
import ctypes
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

LOG_LIMIT_BYTES = 8 * 1024 * 1024
INTERRUPT_GRACE_SECONDS = 5
OUTPUT_POLL_SECONDS = 0.05
WINDOWS_UTF8_CODE_PAGE = 65001


def wants_color(environment, stream):
    return (stream.isatty() and not environment.get("NO_COLOR")
            and environment.get("CLICOLOR") != "0" and environment.get("TERM") != "dumb")


@contextlib.contextmanager
def terminal_color(environment):
    enabled = wants_color(environment, sys.stdout)
    restore = None
    if enabled and os.name == "nt" and sys.stdout.isatty():
        import msvcrt
        from ctypes import wintypes

        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        api.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        handle = msvcrt.get_osfhandle(sys.stdout.fileno())
        original = wintypes.DWORD()
        if api.GetConsoleMode(handle, ctypes.byref(original)):
            # Enable ANSI rendering only for this invocation, restoring the
            # caller's mode even on failure/cancellation. Code pages are untouched.
            virtual_terminal = original.value | 0x0004
            if api.SetConsoleMode(handle, virtual_terminal):
                restore = (api, handle, original.value)
            else:
                enabled = False
        else:
            enabled = False
    try:
        yield enabled
    finally:
        if restore:
            api, handle, original = restore
            api.SetConsoleMode(handle, original)


def color_environment(environment, enabled):
    result = dict(environment)
    result["CLICOLOR"] = "1" if enabled else "0"
    # Ninja otherwise strips compiler ANSI sequences because its output is piped.
    result["CLICOLOR_FORCE"] = "1" if enabled else "0"
    if enabled:
        result.pop("NO_COLOR", None)
    else:
        result["NO_COLOR"] = "1"
    return result


class AnsiStripper:
    """Strip streamed CSI/OSC controls with constant state, even across reads."""

    def __init__(self):
        self.state = "text"

    def strip(self, text):
        output = []
        for character in text:
            if self.state == "text":
                if character == "\x1b":
                    self.state = "escape"
                else:
                    output.append(character)
            elif self.state == "escape":
                if character == "[":
                    self.state = "csi"
                elif character in "]PX^_":
                    self.state = "string"
                else:
                    self.state = "text"
            elif self.state == "csi":
                if "@" <= character <= "~":
                    self.state = "text"
                elif character == "\x1b":
                    self.state = "escape"
            elif self.state == "string":
                if character == "\x07":
                    self.state = "text"
                elif character == "\x1b":
                    self.state = "string_escape"
            elif self.state == "string_escape":
                self.state = "text" if character == "\\" else "string"
        return "".join(output)


def display(text, *, end="\n", file=None):
    stream = file or sys.stdout
    encoding = getattr(stream, "encoding", None) or "utf-8"
    printable = text.encode(encoding, errors="replace").decode(encoding)
    print(printable, end=end, file=stream, flush=True)


@contextlib.contextmanager
def build_lock(directory):
    # Keep the inode/file permanently: unlinking after unlock could give two
    # contenders different locks. OS ownership disappears on crash/termination.
    with (directory / ".vinc-build.lock").open("a+b") as handle:
        handle.seek(0, os.SEEK_END)
        if handle.tell() == 0:
            handle.write(b"\0")
            handle.flush()

        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise RuntimeError("Another build wrapper owns " + str(directory))

        try:
            yield
        finally:
            handle.seek(0)
            if os.name == "nt":
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle, fcntl.LOCK_UN)


class WindowsJob:
    """Own only the gated build worker and its descendants, never the caller."""

    def __init__(self):
        from ctypes import wintypes

        class BasicLimits(ctypes.Structure):
            _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                        ("flags", wintypes.DWORD), ("minimum", ctypes.c_size_t),
                        ("maximum", ctypes.c_size_t), ("active", wintypes.DWORD),
                        ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD),
                        ("scheduling", wintypes.DWORD)]

        class Limits(ctypes.Structure):
            _fields_ = [("basic", BasicLimits), ("io", ctypes.c_uint64 * 6),
                        ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                        ("peak_process", ctypes.c_size_t), ("peak_job", ctypes.c_size_t)]

        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        self.api.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        self.api.CreateJobObjectW.restype = wintypes.HANDLE
        self.api.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int,
                                                    ctypes.c_void_p, wintypes.DWORD]
        self.api.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self.api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.api.OpenProcess.restype = wintypes.HANDLE
        self.api.CloseHandle.argtypes = [wintypes.HANDLE]
        self.api.PeekNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                          ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        self.handle = self.api.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())

        limits = Limits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.api.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            error = ctypes.WinError(ctypes.get_last_error())
            self.close()
            raise error

    def attach(self, process):
        # AssignProcessToJobObject requires SET_QUOTA | TERMINATE access.
        process_handle = self.api.OpenProcess(0x0101, False, process.pid)
        if not process_handle:
            raise ctypes.WinError(ctypes.get_last_error())

        try:
            if not self.api.AssignProcessToJobObject(self.handle, process_handle):
                raise ctypes.WinError(ctypes.get_last_error())
        finally:
            self.api.CloseHandle(process_handle)

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None

    def output_ready(self, stream):
        return windows_pipe_ready(self.api, stream)


def windows_pipe_ready(api, stream):
    """Peek without consuming bytes; a closed pipe is readable as EOF."""
    import msvcrt
    from ctypes import wintypes

    available = wintypes.DWORD()
    handle = msvcrt.get_osfhandle(stream.fileno())
    if not api.PeekNamedPipe(handle, None, 0, None, ctypes.byref(available), None):
        error = ctypes.get_last_error()
        if error == 109:  # ERROR_BROKEN_PIPE: the next read observes EOF.
            return True

        raise ctypes.WinError(error)

    return available.value != 0


def windows_worker(command):
    """Internal gated worker; the parent Job owns this process and all children."""
    from ctypes import wintypes

    # The first byte authorizes spawning only after Job assignment. Keep stdin
    # unbuffered: a subsequent cancellation byte must remain visible to Peek.
    control = sys.stdin.buffer
    if os.read(control.fileno(), 1) != b"1":
        return 1

    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.SetConsoleCP.argtypes = [wintypes.UINT]
    api.SetConsoleOutputCP.argtypes = [wintypes.UINT]
    api.PeekNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                  ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    if not api.SetConsoleCP(WINDOWS_UTF8_CODE_PAGE) or not api.SetConsoleOutputCP(WINDOWS_UTF8_CODE_PAGE):
        raise ctypes.WinError(ctypes.get_last_error())

    # This console belongs only to this worker. CMake/Ninja/MSVC inherit its
    # encoding; source /utf-8 and Python decoding alone cannot establish this.
    child = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    cancellation_sent = False
    while child.poll() is None:
        if not cancellation_sent and windows_pipe_ready(api, control):
            os.read(control.fileno(), 1)
            cancellation_sent = True
            try:
                # Only a sender in the same console can signal this child group.
                child.send_signal(signal.CTRL_BREAK_EVENT)
            except OSError:
                # The child may have exited. The parent still owns forced cleanup.
                pass

        time.sleep(OUTPUT_POLL_SECONDS)

    return child.returncode


def start_windows_worker(command, source, environment):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0  # SW_HIDE: no window and no changes to the caller's console.
    return subprocess.Popen([sys.executable, "-B", "-X", "utf8", str(Path(__file__).resolve())] + command,
                            cwd=source, env=environment, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)


def output_ready(process, job):
    # A blocking Windows pipe read can postpone Python's Ctrl+C handler until
    # another byte arrives. Poll only while a build is active; no reader thread
    # or unbounded output queue is needed. POSIX select also wakes on EOF.
    if job:
        if job.output_ready(process.stdout):
            return True

        time.sleep(OUTPUT_POLL_SECONDS)
        return False

    import select
    readable, _, _ = select.select([process.stdout], [], [], OUTPUT_POLL_SECONDS)
    return bool(readable)


class BuildLog:
    """Stream full output; retain a bounded prefix of the latest invocation."""

    def __init__(self, handle, *, color=False):
        self.handle = handle
        self.remaining = LOG_LIMIT_BYTES
        self.color = color
        self.stripper = AnsiStripper()

    def write(self, text):
        # Windows redirected consoles may still use cp950. A diagnostic glyph
        # must not abort an otherwise valid build; the UTF-8 log keeps the text.
        plain = self.stripper.strip(text)
        display(text if self.color else plain, end="")
        data = plain.encode("utf-8", errors="replace")
        if self.remaining:
            # Trim a partial final code point before adding the truncation marker.
            prefix = data[:self.remaining].decode("utf-8", errors="ignore").encode("utf-8")
            self.handle.write(prefix)
            self.remaining = max(0, self.remaining - len(data))
            if not self.remaining:
                self.handle.write(b"\n[Log limit reached; later output is console-only.]\n")

            self.handle.flush()

    def finish_step(self):
        # Do not let a truncated escape from a failed command eat the next
        # command's text or leave subsequent terminal messages colored.
        self.stripper.state = "text"
        if self.color:
            display("\x1b[0m", end="")


def run_step(command, source, environment, log):
    log.write("\n> " + subprocess.list2cmdline(command) + "\n")
    job = WindowsJob() if os.name == "nt" else None
    process = None
    try:
        if job:
            # The worker cannot start CMake until it belongs to the Job. This avoids
            # the spawn/assignment race where a fast child escapes tree cleanup.
            process = start_windows_worker(command, source, environment)
            job.attach(process)
            os.write(process.stdin.fileno(), b"1")
        else:
            process = subprocess.Popen(command, cwd=source, env=environment, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)

        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        while True:
            if not output_ready(process, job):
                continue

            data = process.stdout.read1(8192)
            if not data:
                break

            log.write(decoder.decode(data))

        log.write(decoder.decode(b"", final=True))
        return process.wait()
    except KeyboardInterrupt:
        log.write("\nInterrupted; stopping this build's process tree.\n")
        if process and process.poll() is None:
            try:
                if job:
                    # The worker owns a different console; ask it to deliver the
                    # graceful signal there instead of altering our own console.
                    os.write(process.stdin.fileno(), b"0")
                else:
                    os.killpg(process.pid, signal.SIGINT)
            except OSError:
                # An already exited worker may have closed the control pipe.
                # The owned Job/process group is still forcibly closed below.
                pass

            try:
                process.wait(timeout=INTERRUPT_GRACE_SECONDS)
            except subprocess.TimeoutExpired:
                pass

        raise
    finally:
        if job:
            job.close()
        elif process:
            # The process group belongs only to this step, including grandchildren.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

        if process:
            if process.poll() is None:
                process.kill()

            process.wait()
            if process.stdout:
                process.stdout.close()
            if process.stdin:
                process.stdin.close()
        log.finish_step()


if __name__ == "__main__":
    try:
        sys.exit(windows_worker(sys.argv[1:]))
    except (OSError, ValueError) as error:
        display("Windows build worker failed: " + str(error), file=sys.stderr)
        sys.exit(1)

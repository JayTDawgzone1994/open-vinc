"""Color policy, streamed plain logs and real native diagnostic forwarding."""

import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("vinc_build_color_tests", ROOT / "build.py")
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)
PROCESS = BUILD.process


def run_with_terminal(command, cwd, environment):
    import errno
    import pty
    import select
    import time

    master, slave = pty.openpty()
    process = None
    try:
        process = subprocess.Popen(command, cwd=cwd, env=environment,
                                   stdin=subprocess.DEVNULL, stdout=slave, stderr=slave)
        os.close(slave)
        slave = None
        output = bytearray()
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if not select.select([master], [], [], 0.1)[0]:
                continue
            try:
                chunk = os.read(master, 8192)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
            if not chunk:
                break
            output.extend(chunk)
        else:
            raise AssertionError("terminal color fixture timed out")
        return subprocess.CompletedProcess(command, process.wait(timeout=10), bytes(output), b"")
    finally:
        if slave is not None:
            os.close(slave)
        os.close(master)
        if process and process.poll() is None:
            import signal
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


class ColorTests(unittest.TestCase):
    def test_auto_requires_terminal_and_honors_opt_out(self):
        terminal = mock.Mock()
        terminal.isatty.return_value = True
        self.assertTrue(PROCESS.wants_color({}, terminal))
        for environment in ({"NO_COLOR": "1"}, {"CLICOLOR": "0"}, {"TERM": "dumb"}):
            self.assertFalse(PROCESS.wants_color(environment, terminal))
        terminal.isatty.return_value = False
        self.assertFalse(PROCESS.wants_color({"CLICOLOR_FORCE": "1"}, terminal))

    def test_color_option_removed(self):
        for mode in ("auto", "always", "never"):
            with self.subTest(mode=mode), mock.patch("sys.stderr", io.StringIO()):
                with self.assertRaises(SystemExit):
                    BUILD.arguments(["--color", mode])
        with mock.patch("sys.stdout", io.StringIO()) as output:
            with self.assertRaises(SystemExit):
                BUILD.arguments(["--help"])
        self.assertNotIn("--color", output.getvalue())

    def test_child_color_environment_does_not_modify_parent(self):
        original = {"NO_COLOR": "1", "CLICOLOR_FORCE": "7", "PATH": "keep"}
        enabled = PROCESS.color_environment(original, True)
        self.assertEqual(enabled["CLICOLOR_FORCE"], "1")
        self.assertNotIn("NO_COLOR", enabled)
        self.assertEqual(original["NO_COLOR"], "1")
        disabled = PROCESS.color_environment(enabled, False)
        self.assertEqual(disabled["CLICOLOR_FORCE"], "0")
        self.assertEqual(disabled["NO_COLOR"], "1")
        self.assertEqual(disabled["PATH"], "keep")

    def test_streamed_controls_are_removed_at_every_split(self):
        colored = "中\x1b[1;31merror\x1b[0m \x1b]8;;https://example.test\x1b\\link\x1b]8;;\x07\n"
        for split in range(len(colored) + 1):
            for enabled in (False, True):
                output = io.BytesIO()
                with self.subTest(split=split, enabled=enabled), mock.patch("sys.stdout", io.StringIO()) as console:
                    log = PROCESS.BuildLog(output, color=enabled)
                    log.write(colored[:split])
                    log.write(colored[split:])
                    self.assertEqual(console.getvalue(), colored if enabled else "中error link\n")
                self.assertEqual(output.getvalue().decode("utf-8"), "中error link\n")

    def test_log_budget_counts_plain_utf8_bytes(self):
        output = io.BytesIO()
        with mock.patch.object(PROCESS, "LOG_LIMIT_BYTES", 4), mock.patch("sys.stdout", io.StringIO()):
            log = PROCESS.BuildLog(output, color=True)
            log.write("\x1b[31m中ab\x1b[0m")
        self.assertTrue(output.getvalue().decode("utf-8").startswith("中a\n[Log limit"))
        self.assertNotIn(b"\x1b", output.getvalue())

    def test_partial_control_cannot_hide_next_step(self):
        output = io.BytesIO()
        with mock.patch("sys.stdout", io.StringIO()):
            log = PROCESS.BuildLog(output)
            log.write("error\x1b[31")
            log.finish_step()
            log.write("next")
        self.assertEqual(output.getvalue(), b"errornext")

    def test_cmake_version_boundary_and_explicit_override(self):
        args = BUILD.arguments([])
        with mock.patch("sys.stdout", io.StringIO()) as output:
            self.assertIsNone(BUILD.compiler_color_definition(args, (3, 20), True))
        self.assertIn("3.24", output.getvalue())
        self.assertEqual(BUILD.compiler_color_definition(args, (3, 24), True), "-DCMAKE_COLOR_DIAGNOSTICS=ON")
        self.assertEqual(BUILD.compiler_color_definition(args, (4, 3), False), "-DCMAKE_COLOR_DIAGNOSTICS=OFF")
        args.cache_values["CMAKE_COLOR_DIAGNOSTICS"] = "OFF"
        self.assertIsNone(BUILD.compiler_color_definition(args, (4, 3), True))

    @unittest.skipUnless(os.name == "nt", "Windows console mode restoration")
    def test_windows_vt_mode_is_restored(self):
        controller = (
            "import ctypes,importlib.util,sys\nfrom ctypes import wintypes\n"
            "spec=importlib.util.spec_from_file_location('p',sys.argv[1])\n"
            "p=importlib.util.module_from_spec(spec)\nspec.loader.exec_module(p)\n"
            "import msvcrt\n"
            "api=ctypes.WinDLL('kernel32')\n"
            "api.GetConsoleMode.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.DWORD)]\n"
            "api.SetConsoleMode.argtypes=[wintypes.HANDLE,wintypes.DWORD]\n"
            "with open('CONOUT$','w') as terminal:\n"
            "    handle=msvcrt.get_osfhandle(terminal.fileno())\n"
            "    mode=wintypes.DWORD()\n"
            "    assert api.GetConsoleMode(handle,ctypes.byref(mode))\n"
            "    original=mode.value & ~4\n"
            "    assert api.SetConsoleMode(handle,original)\n"
            "    saved=sys.stdout\n    sys.stdout=terminal\n"
            "    try:\n"
            "        with p.terminal_color({}) as enabled:\n"
            "            assert enabled\n"
            "            assert api.GetConsoleMode(handle,ctypes.byref(mode)) and mode.value & 4\n"
            "            raise RuntimeError('cancel')\n"
            "    except RuntimeError:\n        pass\n"
            "    finally:\n        sys.stdout=saved\n"
            "    assert api.GetConsoleMode(handle,ctypes.byref(mode)) and mode.value==original\n")
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        result = subprocess.run([sys.executable, "-B", "-c", controller, str(ROOT / "tools/build_process.py")],
                                capture_output=True, timeout=20, startupinfo=startup,
                                creationflags=subprocess.CREATE_NEW_CONSOLE)
        self.assertEqual(result.returncode, 0, result.stderr)


@unittest.skipUnless(os.environ.get("VINC_BUILD_TEST_CMAKE") == "1", "opt-in real color integration")
class NativeColorTests(unittest.TestCase):
    def test_cmake_ninja_and_compiler_colors_with_plain_logs(self):
        with tempfile.TemporaryDirectory(prefix="vinc-color-") as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            binary = root / "binary"
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(colors LANGUAGES CXX)\n'
                'message(WARNING "cmake-color-marker")\n'
                'add_executable(probe main.cpp)\n', encoding="utf-8")
            modes = ["pipe", "file"]
            if os.name != "nt":
                modes.append("terminal")
            for mode in modes:
                (source / "main.cpp").write_text(
                    '#ifdef _MSC_VER\n#pragma message("compiler-color-marker")\n'
                    '#else\n#warning compiler-color-marker\n#endif\n'
                    'int main() { return 0; }\n// ' + mode + '\n', encoding="utf-8")
                invocation = ([str(ROOT / "build.bat")] if os.name == "nt"
                              else ["sh", str(ROOT / "build.sh")])
                environment = dict(os.environ)
                environment["VINC_BUILD_INTERPRETER"] = sys.executable
                command = invocation + ["--source", str(source), "--build-dir", str(binary)]
                if mode == "terminal":
                    environment.pop("NO_COLOR", None)
                    environment.pop("CLICOLOR", None)
                    environment["TERM"] = "xterm"
                    result = run_with_terminal(command, root, environment)
                elif mode == "file":
                    destination = root / "redirected.txt"
                    destination.write_bytes(b"previous output\n")
                    with destination.open("ab") as output:
                        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                                env=environment, timeout=120)
                    result.stdout = destination.read_bytes()
                    result.stderr = b""
                    self.assertTrue(result.stdout.startswith(b"previous output\n"))
                else:
                    result = subprocess.run(command, capture_output=True, env=environment, timeout=120)
                with self.subTest(mode=mode):
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    log = (binary / ".vinc-build.log").read_bytes()
                    self.assertIn(b"cmake-color-marker", log)
                    self.assertIn(b"compiler-color-marker", log)
                    self.assertNotIn(b"\x1b", log)
                    if mode == "terminal":
                        self.assertIn(b"\x1b[", result.stdout)
                        if os.name != "nt":
                            compiler_lines = [line for line in result.stdout.splitlines() if b"warning:" in line]
                            self.assertTrue(any(b"\x1b[" in line for line in compiler_lines), result.stdout)
                    else:
                        self.assertNotIn(b"\x1b", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

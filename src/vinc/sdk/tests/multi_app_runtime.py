"""Bounded, real-process admission and isolation checks for the App fixture."""

import json
import os
import re
import signal
import socket
import subprocess
import time
import urllib.request


WAIT_SECONDS = 30
POLL_SECONDS = 0.05
IO_SECONDS = 10
MAX_REPLY_BYTES = 1024 * 1024


class RunningApp:
    """Own only this invocation and its children; never kill by executable name."""

    def __init__(self, executable, python, log):
        self.log = log
        self.stream = log.open("wb")
        self.socket = None
        self.pending = b""
        try:
            self.process = subprocess.Popen(
                [str(executable), "--shell-port", "0", "--python", python],
                stdin=subprocess.DEVNULL, stdout=self.stream, stderr=subprocess.STDOUT,
                env=dict(os.environ, VINC_TEST_KEEP_RUNNING="1"),
                start_new_session=os.name != "nt")
        except BaseException:
            self.stream.close()
            raise

    def wait_log(self, pattern):
        deadline = time.monotonic() + WAIT_SECONDS
        while time.monotonic() < deadline:
            content = self.log.read_text(encoding="utf-8", errors="replace")
            matches = re.findall(pattern, content)
            if matches:
                return matches[-1]
            if self.process.poll() is not None:
                raise RuntimeError(content)
            time.sleep(POLL_SECONDS)
        raise RuntimeError(f"Timed out waiting for {pattern}: {content}")

    def connect(self):
        port = int(self.wait_log(r"Socket Shell listening on 127\.0\.0\.1:(\d+)"))
        self.socket = socket.create_connection(("127.0.0.1", port), IO_SECONDS)
        self.receive_reply()
        return port

    def receive_reply(self):
        marker = b"VINC > "
        deadline = time.monotonic() + IO_SECONDS
        while marker not in self.pending:
            self.socket.settimeout(max(0.001, deadline - time.monotonic()))
            chunk = self.socket.recv(4096)
            if not chunk:
                raise RuntimeError("Shell closed before command completion")
            self.pending += chunk
            if len(self.pending) > MAX_REPLY_BYTES or time.monotonic() >= deadline:
                raise RuntimeError("Shell reply exceeded test bounds")
        end = self.pending.index(marker) + len(marker)
        reply = self.pending[:end].decode("utf-8", errors="replace")
        self.pending = self.pending[end:]
        return reply

    def command(self, text):
        self.socket.sendall((text + "\n").encode())
        return self.receive_reply()

    def stop(self):
        try:
            if self.process.poll() is None:
                if self.socket is None:
                    self.connect()
                self.socket.sendall(b"exit\n")
                self.process.wait(timeout=WAIT_SECONDS)
        finally:
            if self.process.poll() is None:
                if os.name == "nt":
                    subprocess.run(["taskkill", "/PID", str(self.process.pid), "/T", "/F"],
                                   capture_output=True, timeout=IO_SECONDS)
                else:
                    os.killpg(self.process.pid, signal.SIGKILL)
                self.process.kill()
                self.process.wait(timeout=IO_SECONDS)
            if self.socket is not None:
                self.socket.close()
                self.socket = None
            self.stream.close()


def graph(port):
    base = f"http://127.0.0.1:{port}"
    # Loopback requests must not inherit a developer's external HTTP proxy.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    session = urllib.request.Request(base + "/api/session", headers={"X-Vinc-UI": "1"})
    with opener.open(session, timeout=IO_SECONDS) as response:
        token = json.load(response)["token"]
    request = urllib.request.Request(base + "/api/graph",
        headers={"X-Vinc-UI": "1", "Authorization": "Bearer " + token})
    with opener.open(request, timeout=IO_SECONDS) as response:
        return json.load(response)


def verify_coexistence(first, second, python, work, run, require):
    first_app = RunningApp(first, python, work / "coexist-first.log")
    second_app = None
    try:
        first_app.wait_log("First_SCRIPT_OK")
        first_port = first_app.connect()
        duplicate = run([str(first), "--shell-port", "0"], success=False, timeout=WAIT_SECONDS)
        require("A Vinc with this name is already running" in duplicate, duplicate)
        require("Socket Shell listening" not in duplicate, duplicate)

        # Move First's UI away from the default port using existing Shell APIs.
        mid = first_app.wait_log(r"Official Module webUi ready: ui MID=(\d+)")
        require("Module stopped" in first_app.command(f"debug stop {mid}"), "UI stop failed")
        reply = first_app.command("webUi --port 0")
        require("Started ui" in reply, reply)
        ui_port = int(first_app.wait_log(r"Web UI listening on http://127\.0\.0\.1:((?!8080\b)\d+)/"))

        second_app = RunningApp(second, python, work / "coexist-second.log")
        second_app.wait_log("Second_SCRIPT_OK")
        second_port = second_app.connect()
        require(first_port != second_port and ui_port != 8080, "Listener isolation failed")
        first_types = {entry["type"] for entry in graph(ui_port)["modules"]}
        second_types = {entry["type"] for entry in graph(8080)["modules"]}
        require("FirstModule" in first_types and "SecondModule" not in first_types
                and "ScriptPeer" not in first_types, str(first_types))
        require("SecondModule" in second_types and "FirstModule" not in second_types
                and "ScriptPeer" in second_types, str(second_types))
        first_app.stop()
        require(first_app.process.returncode == 0, "First shutdown failed")
        require("All module processes exited" in first_app.log.read_text(encoding="utf-8"),
                "First did not confirm child cleanup")
        reply = second_app.command("recorder status")
        require("Error:" not in reply, reply)
        require("SecondModule" in {entry["type"] for entry in graph(8080)["modules"]},
                "Stopping First affected Second")

        restarted = run([str(first), "--shell-port", "0"], timeout=WAIT_SECONDS,
                        env=dict(os.environ, VINC_TEST_UI_UNAVAILABLE="1"))
        require("First_SCRIPT_OK" in restarted and "APP_TEST_FAILED" not in restarted, restarted)
        second_app.stop()
        require(second_app.process.returncode == 0 and "All module processes exited" in
                second_app.log.read_text(encoding="utf-8"), "Second shutdown failed")
        print("PASS: same-name exclusion, different-App concurrency, UI graph and shutdown isolation", flush=True)
    finally:
        try:
            if second_app is not None:
                second_app.stop()
        finally:
            first_app.stop()

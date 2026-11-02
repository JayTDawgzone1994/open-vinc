"""Real official plugins: protection, HTTP toggle, crash recovery and shutdown."""

import importlib.util
import os
from pathlib import Path
import re
import signal
import sqlite3
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "app_runtime", ROOT / "src/vinc/sdk/tests/multi_app_runtime.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
RECOVERY_SECONDS = 80  # Includes one complete configured start-limit window.


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def expect_reply(app, command, text):
    # Central fault notices redraw the prompt too. They are not the reply to our
    # command, so wait for its identifying payload rather than counting prompts.
    reply = app.command(command)
    deadline = time.monotonic() + runtime.IO_SECONDS
    while text not in reply:
        require(time.monotonic() < deadline, f"Missing reply {text}: {reply}")
        reply += app.receive_reply()
    return reply


def snapshot(app, name):
    reply = expect_reply(app, "help --verbose", "[System Modules]")
    match = re.search(rf"^  {name} (\w+) MID=(\d+) PID=(\d+)([^\r\n]*)", reply, re.M)
    require(match, reply)
    state, mid, pid, detail = match.groups()
    return state, int(mid), int(pid), detail


def wait_running(app, name, previous_mid=0):
    deadline = time.monotonic() + RECOVERY_SECONDS
    while time.monotonic() < deadline:
        current = snapshot(app, name)
        if current[0] == "Running" and current[1] != previous_mid:
            return current
        time.sleep(runtime.POLL_SECONDS)
    raise RuntimeError(f"Recovery deadline: {name}: {current}")


def wait_output(app, text, start):
    deadline = time.monotonic() + runtime.WAIT_SECONDS
    while time.monotonic() < deadline:
        content = app.log.read_text(encoding="utf-8", errors="replace")[start:]
        if text in content:
            return
        time.sleep(runtime.POLL_SECONDS)
    raise RuntimeError(f"Missing output {text}: {content}")


def command_output(app, command, text):
    start = len(app.log.read_text(encoding="utf-8", errors="replace"))
    reply = app.command(command)
    require("Error:" not in reply, reply)
    wait_output(app, text, start)


def recording_completed(directory):
    database = directory / "recording.sqlite"
    require(database.exists(), "Recording database missing")
    connection = sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)
    try:
        return connection.execute("SELECT completed FROM recording WHERE id=1").fetchone()[0] == 1
    finally:
        connection.close()


def crash(app, name, expected):
    # Only kill the currently confirmed child of this test's own central. Never
    # find a process by name or use a stale PID retained across a restart.
    current = snapshot(app, name)
    require(current[:3] == expected[:3] and current[0] == "Running", str(current))
    require(current[2] > 0 and current[2] != app.process.pid, "Invalid child PID")
    os.kill(current[2], signal.SIGTERM)


def verify(executable, work):
    app = runtime.RunningApp(executable, sys.executable, work / "system-services.log")
    try:
        app.connect()
        app.wait_log("VINC ready")
        help_text = expect_reply(app, "help", "[Commands]")
        require("webUi" not in help_text and "bagRecorder" not in help_text, help_text)
        require("ui Running" not in help_text and "recorder Running" not in help_text, help_text)
        require("recorder" in help_text and "ui" in help_text, help_text)
        expect_reply(app, "ui -h", "Usage: ui")
        expect_reply(app, "recorder --help", "Usage: recorder")

        # Disable is also accepted while waiting to retry after an occupied port.
        expect_reply(app, "ui disable", "UI disable requested")
        first_ui = wait_running(app, "ui")
        require("desired=disabled" in first_ui[3], str(first_ui))
        command_output(app, "ui status", "UI running=0")
        crash(app, "ui", first_ui)
        next_ui = wait_running(app, "ui", first_ui[1])
        require("desired=disabled" in next_ui[3], str(next_ui))
        command_output(app, "ui status", "UI running=0")

        recorder = wait_running(app, "recorder")
        command_output(app, "recorder status", "bag: idle")
        bag = (work / "interrupted").as_posix()
        reply = app.command(f'recorder record "{bag}" --all')
        require("Error:" not in reply, reply)
        command_output(app, "recorder status", "bag: accepting=1")
        crash(app, "recorder", recorder)
        restarted = wait_running(app, "recorder", recorder[1])
        command_output(app, "recorder status", "bag: idle")
        require(not recording_completed(work / "interrupted"), "Crash cannot mark the Bag complete")

        for name, current, module_type in (("ui", next_ui, "webUi"),
                                           ("recorder", restarted, "bagRecorder")):
            for command in (f"debug stop {current[1]}", module_type,
                            f"debug start {module_type}"):
                expect_reply(app, command, "System Module")
                details = expect_reply(app, "shell -e", "domain=")
                require("SystemModuleProtected" in details or "System Module" in details, details)
            require(snapshot(app, name)[:3] == current[:3], "Protected command changed process")

        shutdown_bag = (work / "shutdown").as_posix()
        app.command(f'recorder record "{shutdown_bag}" --all')
        command_output(app, "recorder status", "bag: accepting=1")
        app.stop()
        require(app.process.returncode == 0, "Central shutdown failed")
        require("All module processes exited" in app.log.read_text(encoding="utf-8"),
                "Central did not confirm child cleanup")
        require(recording_completed(work / "shutdown"), "Normal shutdown finishes recording")
        print("PASS: official catalog, hidden listings, commands, protected lifecycle, "
              "UI disabled intent after crash, Recorder idle after crash, full shutdown")
    finally:
        app.stop()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="vinc-system-services-") as directory:
        verify(Path(sys.argv[1]).resolve(), Path(directory))

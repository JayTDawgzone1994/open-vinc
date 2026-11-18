"""Run two relocated Apps and validate committed generic images with a typed decoder."""

import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys


APP_TIMEOUT_SECONDS = 60
RECORD_COUNT = 64


def verify_bag(app, decoder, work):
    bag = app.parent / "recorded" / "recording.sqlite"
    with sqlite3.connect(bag.as_uri() + "?mode=ro", uri=True) as database:
        assert database.execute("PRAGMA user_version").fetchone() == (3,)
        state = database.execute(
            "SELECT completed,received,written,queue_dropped,oversized,bus_lost FROM recording").fetchone()
        assert state == (1, RECORD_COUNT, RECORD_COUNT, 0, 0, 0), state
        topics = database.execute("SELECT name,type,type_known FROM topics").fetchall()
        assert topics == [("sdk/recorder", "RecorderSample", 1)], topics
        rows = database.execute(
            "SELECT publisher,publisher_id,sequence,source_utc_ns,source_monotonic_ns,"
            "receive_monotonic_ns,payload FROM messages ORDER BY id").fetchall()
    assert len(rows) == RECORD_COUNT
    images = work / (app.stem + "-images")
    images.mkdir()
    identities = set()
    for sequence, row in enumerate(rows, 1):
        publisher, identity, count, utc, monotonic, received, payload = row
        assert publisher == "probe" and int(identity) != 0
        identities.add(identity)
        assert int(count) == sequence and int(utc) > 0 and 0 < int(monotonic) <= int(received)
        (images / f"{sequence}.bin").write_bytes(payload)
    assert len(identities) == 1
    result = subprocess.run([str(decoder), str(images)], capture_output=True, text=True, timeout=20)
    (work / (app.stem + "-decode.log")).write_text(result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode == 0 and "INSTALLED_RECORDER_CONTENT_OK" in result.stdout


def main():
    work, decoder, *apps = map(Path, sys.argv[1:])
    running = []
    try:
        for app in apps:
            log = (work / (app.stem + "-run.log")).open("wb")
            process = subprocess.Popen([str(app), "--shell-port", "0"],
                                       cwd=work / "unrelated cwd", stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=os.name != "nt")
            running.append((app, process, log))
        for app, process, log in running:
            assert process.wait(timeout=APP_TIMEOUT_SECONDS) == 0
            log.flush()
            output = (work / (app.stem + "-run.log")).read_text("utf-8", errors="replace")
            assert "INSTALLED_RECORDER_FAILED" not in output, output
            assert "INSTALLED_RECORDER_WAIT_OK" in output, output
            assert "All module processes exited" in output, output
            verify_bag(app, decoder, work)
    finally:
        for _, process, log in running:
            if process.poll() is None:
                if os.name == "nt":
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                   capture_output=True, timeout=10)
                else:
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=10)
            log.close()
    print("INSTALLED_RECORDER_MULTI_APP_CONTENT_OK")


if __name__ == "__main__":
    main()

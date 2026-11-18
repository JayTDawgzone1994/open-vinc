"""Record identical paced workloads through real Hosts and compare committed images."""

import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sqlite3
import sys
import threading
import time


RECORDS = 1000
SAMPLE_SECONDS = 0.02
WAIT_SECONDS = 60
IDLE_SECONDS = 1
BURST_COMMANDS = 10
REPETITIONS = 3
QUEUE_BYTES = 64 * 1024 * 1024
MESSAGE_BYTES = 128 * 1024


class Usage:
    def __init__(self, pid):
        self.pid = pid
        self.handle = None
        if os.name == "nt":
            from ctypes import wintypes

            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            self.kernel.OpenProcess.restype = wintypes.HANDLE
            self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            self.kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
            self.handle = self.kernel.OpenProcess(0x1000 | 0x0010, False, pid)
            if not self.handle:
                raise ctypes.WinError(ctypes.get_last_error())
            self.psapi = ctypes.WinDLL("psapi", use_last_error=True)
            self.psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]

    def read(self):
        if os.name != "nt":
            values = Path(f"/proc/{self.pid}/stat").read_text().rsplit(")", 1)[1].split()
            cpu = (int(values[11]) + int(values[12])) / os.sysconf("SC_CLK_TCK")
            rss = int(values[21]) * os.sysconf("SC_PAGE_SIZE")
            return cpu, rss
        from ctypes import wintypes

        values = [wintypes.FILETIME() for _ in range(4)]
        if not self.kernel.GetProcessTimes(self.handle, *[ctypes.byref(value) for value in values]):
            raise ctypes.WinError(ctypes.get_last_error())
        cpu = sum((value.dwHighDateTime << 32) + value.dwLowDateTime for value in values[2:]) / 10_000_000
        # PROCESS_MEMORY_COUNTERS_EX: two DWORDs followed by nine SIZE_T fields.
        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD),
                        ("sizes", ctypes.c_size_t * 9)]

        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        if not self.psapi.GetProcessMemoryInfo(self.handle, ctypes.byref(counters), counters.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        return cpu, counters.sizes[1]

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def recorded(database, completed=False):
    if not database.exists():
        return False
    with sqlite3.connect(database.as_uri() + "?mode=ro", uri=True, timeout=2) as connection:
        row = connection.execute("SELECT completed,received,written,queue_dropped,oversized,bus_lost FROM recording").fetchone()
        return row and row[1:] == (RECORDS, RECORDS, 0, 0, 0) and (not completed or row[0] == 1)


def until(condition):
    end = time.monotonic() + WAIT_SECONDS
    while time.monotonic() < end:
        if condition():
            return
        time.sleep(SAMPLE_SECONDS)
    raise RuntimeError("Cost workload did not reach its expected state")


def successful_command(reply):
    return reply.replace("\r", "").strip() in ("OK\nVINC >", "VINC >")


def measure(helper, work, variant, size, repetition, run_name):
    suffix = ".exe" if os.name == "nt" else ""
    app = work / (variant + "-build") / "output/CostApp/Release" / ("CostApp" + suffix)
    prefix = f"{run_name}-{variant}-{size}-{repetition}"
    folder = work / prefix
    running = helper.RunningApp(app, sys.executable, work / (prefix + "-run.log"))
    usage = None
    stop_sampling = threading.Event()
    sampler = None
    samples = []
    sample_errors = []
    replies = []
    try:
        running.connect()

        def command(text):
            reply = running.command(text)
            replies.append((text, reply))
            return reply

        command("ui disable")
        command("output off")
        states = command("help --verbose")
        pid = int(re.search(r"recorder Running MID=\d+ PID=(\d+)", states)[1])
        usage = Usage(pid)
        idle_start = usage.read()[0]
        time.sleep(IDLE_SECONDS)
        idle_cpu = usage.read()[0] - idle_start
        command("RecorderProbe")
        command(f'recorder record "{folder.as_posix()}" --topic sdk/recorder '
                f'--queue-bytes {QUEUE_BYTES} --message-bytes {MESSAGE_BYTES}')
        until(lambda: (folder / "recording.sqlite").exists())
        until(lambda: successful_command(command("probe ready")))

        def sample():
            try:
                while not stop_sampling.wait(SAMPLE_SECONDS):
                    samples.append(usage.read())
            except BaseException as error:
                sample_errors.append(str(error))

        initial_cpu, initial_rss = usage.read()
        started = time.perf_counter()
        sampler = threading.Thread(target=sample)
        sampler.start()
        # Bound each Shell command below its normal five-second request deadline.
        for _ in range(BURST_COMMANDS):
            reply = command("probe burst " + size)
            if not successful_command(reply):
                raise RuntimeError(reply)
        published = time.perf_counter()
        database = folder / "recording.sqlite"
        until(lambda: recorded(database))
        drained = time.perf_counter()
        command("recorder finish")
        until(lambda: recorded(database, completed=True))
        finished = time.perf_counter()
        final_cpu, final_rss = usage.read()
        stop_sampling.set()
        sampler.join()
        if sample_errors:
            raise RuntimeError(sample_errors)
        with sqlite3.connect(database.as_uri() + "?mode=ro", uri=True) as connection:
            rows = connection.execute("SELECT sequence,publisher,publisher_id,payload FROM messages ORDER BY id").fetchall()
        if len(rows) != RECORDS or [int(row[0]) for row in rows] != list(range(1, RECORDS + 1)):
            raise RuntimeError("Recording lost order or records")
        if any(row[1] != "probe" or int(row[2]) == 0 for row in rows):
            raise RuntimeError("Publisher metadata was not preserved")
        digest = hashlib.sha256()
        for row in rows:
            digest.update(row[3])
        return {"variant": variant, "size": size, "repetition": repetition,
                "records": RECORDS, "payload_digest": digest.hexdigest(),
                "publish_seconds": published - started, "drain_seconds": drained - published,
                "finish_seconds": finished - drained, "records_per_second": RECORDS / (drained - started),
                "recorder_cpu_seconds": final_cpu - initial_cpu, "idle_cpu_seconds_per_second": idle_cpu,
                "sampled_peak_rss_bytes": max([initial_rss, final_rss, *[entry[1] for entry in samples]]),
                "sample_count": len(samples), "database_bytes": database.stat().st_size,
                "queue_dropped": 0, "bus_lost": 0}
    finally:
        stop_sampling.set()
        if sampler:
            sampler.join(timeout=5)
        if usage:
            usage.close()
        running.stop()
        (work / (prefix + "-commands.json")).write_text(json.dumps(replies, indent=2), encoding="utf-8")
        if running.process.returncode != 0 or "All module processes exited" not in running.log.read_text("utf-8", errors="replace"):
            raise RuntimeError("Cost App did not complete process cleanup")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("repository", type=Path)
    parser.add_argument("work", type=Path)
    parser.add_argument("--run-name", default="runtime")
    args = parser.parse_args()
    if os.name != "nt" and sys.platform != "linux":
        parser.error("Recorder CPU/RSS sampling currently supports Windows and Linux only.")
    spec = importlib.util.spec_from_file_location("runtime_fixture", args.repository / "src/vinc/sdk/tests/multi_app_runtime.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    helper.IO_SECONDS = WAIT_SECONDS
    results = []
    for size in ("small", "large"):
        for repetition in range(REPETITIONS):
            variants = ("typed", "generic") if repetition % 2 == 0 else ("generic", "typed")
            pair = [measure(helper, args.work.resolve(), variant, size, repetition, args.run_name) for variant in variants]
            if pair[0]["payload_digest"] != pair[1]["payload_digest"]:
                raise RuntimeError("Typed/generic committed payloads differ")
            results.extend(pair)
            (args.work / "runtime-results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
            print(f"RUNTIME_COST_OK {size} {repetition}", flush=True)


if __name__ == "__main__":
    main()

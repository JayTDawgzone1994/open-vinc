"""Run the private topology fixture once and report whole-process peak memory.

The fixture prints protocol/storage counters separately. RSS/private commit also
include the test drivers, registry, mappings, CRT and allocator; they are not a
measurement of topology storage alone. No packages or uploads are required.
"""

import argparse
import ctypes
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time


def windows_memory(handle):
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [
            ("size", wintypes.DWORD),
            ("faults", wintypes.DWORD),
            ("peak_working_set", ctypes.c_size_t),
            ("working_set", ctypes.c_size_t),
            ("peak_paged_pool", ctypes.c_size_t),
            ("paged_pool", ctypes.c_size_t),
            ("peak_nonpaged_pool", ctypes.c_size_t),
            ("nonpaged_pool", ctypes.c_size_t),
            ("pagefile", ctypes.c_size_t),
            ("peak_pagefile", ctypes.c_size_t),
            ("private", ctypes.c_size_t),
        ]

    query = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
    query.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    query.restype = wintypes.BOOL
    counters = Counters()
    counters.size = ctypes.sizeof(counters)
    if not query(int(handle), ctypes.byref(counters), counters.size):
        raise ctypes.WinError(ctypes.get_last_error())
    return {
        "peak_working_set_bytes": counters.peak_working_set,
        "peak_private_commit_bytes": counters.peak_pagefile,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--scenario", default="topology-load",
                        choices=("topology-load", "topology-snapshot", "topology-replace"))
    arguments = parser.parse_args()
    executable = arguments.executable.resolve(strict=True)
    started = time.monotonic()
    with subprocess.Popen([str(executable), arguments.scenario], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                          errors="replace") as process:
        try:
            output, _ = process.communicate(timeout=45)
        except subprocess.TimeoutExpired:
            process.kill()  # Only this runner's own fixture process.
            process.communicate()
            raise
        print(output, end="")
        memory = {}
        if os.name == "nt":
            memory = windows_memory(process._handle)
        else:
            import resource
            usage = resource.getrusage(resource.RUSAGE_CHILDREN)
            unit = 1 if sys.platform == "darwin" else 1024
            memory["peak_resident_bytes"] = usage.ru_maxrss * unit
        print(json.dumps({"platform": platform.platform(), "executable": str(executable),
                          "elapsed_seconds": time.monotonic() - started,
                          "exit_code": process.returncode, **memory}))
        return process.returncode


if __name__ == "__main__":
    sys.exit(main())

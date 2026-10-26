"""Sequential, bounded-capacity Message Bus latency benchmark (Python stdlib only)."""

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

MIB = 1024 * 1024
SHARED_BUDGET = 4 * 1024 * MIB
IMAGE_ALLOWANCE = 4096
CONTROL_RESERVE = MIB
SLOT_CONTROL_BYTES = 64
MAX_SLOTS = 65536
INITIAL_SLOTS = 8
PILOT_SAMPLES = 128
MIN_WARMUP = 64
MAX_ATTEMPTS = 12
INITIAL_PERIOD_NS = 10_000_000
MAX_PERIOD_NS = 1_000_000_000


def rows(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return [{key: int(value) for key, value in row.items()} for row in csv.DictReader(stream)]


def write_csv(path, records):
    if not records:
        return
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def distribution(prefix, values):
    if not values:
        return {f"{prefix}_{name}_ms": "" for name in ("p50", "p90", "p99", "max")}
    return {
        f"{prefix}_p50_ms": percentile(values, 0.50) / 1e6,
        f"{prefix}_p90_ms": percentile(values, 0.90) / 1e6,
        f"{prefix}_p99_ms": percentile(values, 0.99) / 1e6,
        f"{prefix}_max_ms": max(values) / 1e6,
    }


def analyze(directory, mode, payload_mib, slots, period, samples, warmup):
    sent_all = rows(directory / "sent.csv")
    received_all = rows(directory / "received.csv")
    publisher = rows(directory / "publisher.csv")[0]
    subscriber = rows(directory / "subscriber.csv")[0]
    received = {row["sequence"]: row for row in received_all}
    sent = [row for row in sent_all if row["sequence"] > warmup]
    expected_sequences = set(range(1, warmup + samples + 1))
    actual_sequences = set(received)
    integrity_errors = sum(not row["valid"] for row in received_all)
    integrity_errors += subscriber.get("excess_callbacks", 0)
    integrity_errors += len(received_all) - len(received)
    integrity_errors += len(actual_sequences - expected_sequences)
    if {row["sequence"] for row in sent_all} != expected_sequences or len(sent_all) != len(expected_sequences):
        raise RuntimeError("sender did not record exactly the expected sequences")
    if publisher["shared_bytes"] + 8 > SHARED_BUDGET:
        raise RuntimeError("actual shared allocation exceeds the 4 GiB budget")

    joined = []
    latencies = []
    full_latencies = []
    arrivals = []
    for row in sent:
        match = received.get(row["sequence"])
        arrival = match["arrival_ns"] if match else None
        latency = arrival - row["begin_ns"] if match else None
        if latency is not None:
            integrity_errors += latency < 0
            latencies.append(latency)
            full_latencies.append(latency + row["prepare_ns"])
            arrivals.append(arrival)
        joined.append({
            **row,
            "arrival_ns": arrival if match else "",
            "latency_ns": latency if match else "",
            "valid": match["valid"] if match else "",
        })
    write_csv(directory / "samples.csv", joined)
    span = max([sent[-1]["end_ns"], *arrivals]) - sent[0]["begin_ns"]
    elapsed = span / 1e9
    achieved_hz = (len(sent) - 1) * 1e9 / (sent[-1]["begin_ns"] - sent[0]["begin_ns"])
    missing_all = len(expected_sequences - actual_sequences)
    clean = missing_all == 0 and subscriber["lost_messages"] == 0 and publisher["collisions"] == 0
    clean = clean and integrity_errors == 0 and publisher["published"] == warmup + samples
    result = {
        "mode": mode,
        "payload_mib": payload_mib,
        "samples": samples,
        "received": len(latencies),
        "missing_measured": samples - len(latencies),
        "missing_percent": (samples - len(latencies)) / samples * 100,
        "missing_including_warmup": missing_all,
        "integrity_errors": integrity_errors,
        "warmup": warmup,
        "slots": slots,
        "shared_bytes": publisher["shared_bytes"],
        "image_bytes": publisher["image_bytes"],
        "requested_hz": 1e9 / period,
        "achieved_hz": achieved_hz,
        "receive_mib_s": len(latencies) * payload_mib / elapsed,
        "measurement_seconds": elapsed,
        "collisions": publisher["collisions"],
        "wait_total_ms": publisher["total_wait_ns"] / 1e6,
        "wait_max_ms": publisher["longest_wait_ns"] / 1e6,
        "lost_messages": subscriber["lost_messages"],
        "overwrite_events": subscriber["overwrite_events"],
        "clean": clean,
        "service_p99_ns": percentile([row["prepare_ns"] + row["end_ns"] - row["begin_ns"] for row in sent], 0.99),
        **distribution("latency", latencies),
        **distribution("send_call", [row["end_ns"] - row["begin_ns"] for row in sent]),
        **distribution("prepare", [row["prepare_ns"] for row in sent]),
        **distribution("full_path", full_latencies),
    }
    quarter = max(1, len(latencies) // 4)
    result["first_quarter_p50_ms"] = percentile(latencies[:quarter], 0.50) / 1e6 if latencies else ""
    result["last_quarter_p50_ms"] = percentile(latencies[-quarter:], 0.50) / 1e6 if latencies else ""
    for role in ("publisher", "subscriber", "central"):
        resource = rows(directory / f"{role}_resources.csv")[0]
        result[f"{role}_cpu_percent_one_core"] = (
            resource["cpu_ns"] / resource["wall_ns"] * 100 if resource["valid"] else ""
        )
        result[f"{role}_peak_rss_mib"] = resource["peak_rss_bytes"] / MIB if resource["valid"] else ""
    result["directory"] = directory.name
    return result


def trial(args, mode, size, slots, period, count, name):
    directory = args.output / name
    directory.mkdir()  # Never silently overwrite previous evidence.
    warmup = max(MIN_WARMUP, slots * 2)
    command = [str(args.exe), str(args.plugin), mode, str(size * MIB), str(slots),
               str(period), str(count), str(warmup), str(directory)]
    timeout = (count + warmup) * period / 1e9 * 5 + 100
    with (directory / "driver.log").open("w", encoding="utf-8") as log:
        process_options = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {
            "start_new_session": True
        }
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, **process_options)
        try:
            code = process.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            # The driver observes this marker and shuts down its own Modules.
            (directory / "cancel").touch()
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                # Host heartbeat supervision detects the central's death.
                time.sleep(7)
            raise
    if code:
        raise RuntimeError(f"trial failed ({code}); inspect {directory / 'driver.log'}")
    return analyze(directory, mode, size, slots, period, count, warmup)


def environment(args):
    info = {
        "platform": platform.platform(), "machine": platform.machine(),
        "python": sys.version, "logical_cpus": os.cpu_count(),
        "processor": platform.processor(), "command": sys.argv,
        "start_local": time.strftime("%Y-%m-%d %H:%M:%S %z"),
        "shared_budget_bytes": SHARED_BUDGET,
        "exe": str(args.exe), "plugin": str(args.plugin),
        "binary_sha256": {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (args.exe, args.plugin)
        },
        "percentile_method": "nearest rank, ceil(p*n); received samples only",
        "resource_window": "per-process startup-to-report; includes warmup and idle setup/drain",
        "rss": "process-lifetime peak; shared pages may be counted by both processes",
    }
    if os.name == "nt":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            info["processor"] = winreg.QueryValueEx(key, "ProcessorNameString")[0]
    cache = args.exe
    for parent in args.exe.parents:
        if (parent / "CMakeCache.txt").exists():
            cache = parent / "CMakeCache.txt"
            break
    if cache.is_file() and cache.name == "CMakeCache.txt":
        lines = cache.read_text(encoding="utf-8").splitlines()
        info["cmake"] = [line for line in lines if line.startswith((
            "CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS_RELEASE:", "CMAKE_GENERATOR:"
        ))]
        compiler_files = list((cache.parent / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
        info["compiler"] = [
            line for compiler in compiler_files for line in compiler.read_text(encoding="utf-8").splitlines()
            if line.startswith(("set(CMAKE_CXX_COMPILER_ID ", "set(CMAKE_CXX_COMPILER_VERSION "))
        ]
    revision = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, check=False)
    info["git_head"] = revision.stdout.strip()
    return info


def report(args, results):
    write_csv(args.output / "summary.csv", results)
    lines = ["# Message Bus benchmark", "", "Latency: send call entry to subscriber callback entry.",
             "Warmup excluded from percentiles. No-collision runs are paced, not saturation throughput.", "",
             "| Mode | MiB | Slots | Hz (actual) | P50 ms | P90 ms | P99 ms | Max ms | MiB/s | Clean |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for row in results:
        lines.append(f"| {row['mode']} | {row['payload_mib']} | {row['slots']} | {row['achieved_hz']:.2f} "
                     f"| {row['latency_p50_ms']:.3f} | {row['latency_p90_ms']:.3f} "
                     f"| {row['latency_p99_ms']:.3f} | {row['latency_max_ms']:.3f} "
                     f"| {row['receive_mib_s']:.2f} | {row['clean']} |")
    lines += ["", "Full prepare/send distributions, resource metrics, loss and collision counters: summary.csv.",
              "Raw timestamps and unsuccessful calibration attempts are retained in individual directories.",
              "CPU includes warmup/setup/drain in each process's own window. 100% = one logical core.",
              "Peak RSS includes shared pages and must not be summed as physical RAM.",
              "Loan preparation includes get_next + resize/zero-fill; push reuses a prepared local message.",
              "128 samples in pilots are calibration only; 1,000 final samples give about 10 upper-tail observations."]
    (args.output / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--plugin", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--sizes", type=int, nargs="+", default=[1, 2, 4, 8, 16, 32, 64, 128])
    parser.add_argument("--modes", choices=["push", "loan"], nargs="+", default=["push", "loan"])
    args = parser.parse_args()
    if not 2 <= args.samples <= 100000 or any(size not in [1, 2, 4, 8, 16, 32, 64, 128] for size in args.sizes):
        parser.error("invalid samples or payload sizes")
    args.exe = args.exe.resolve(strict=True)
    args.plugin = args.plugin.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "environment.json").write_text(json.dumps(environment(args), indent=2), encoding="utf-8")
    results = []
    attempts = []
    for size in args.sizes:
        for mode in args.modes:
            maximum = min(MAX_SLOTS, (SHARED_BUDGET - CONTROL_RESERVE) //
                          (size * MIB + IMAGE_ALLOWANCE + SLOT_CONTROL_BYTES))
            slots = min(INITIAL_SLOTS, maximum)
            period = INITIAL_PERIOD_NS
            for attempt in range(MAX_ATTEMPTS):
                name = f"{mode}-{size}MiB-pilot-{attempt}"
                pilot = trial(args, mode, size, slots, period, PILOT_SAMPLES, name)
                attempts.append(pilot)
                write_csv(args.output / "attempts.csv", attempts)
                print(f"{name}: slots={slots}, requested={1e9/period:.2f} Hz, "
                      f"actual={pilot['achieved_hz']:.2f}, clean={pilot['clean']}", flush=True)
                if pilot["integrity_errors"]:
                    raise RuntimeError("integrity failure; do not tune away corrupt data")
                # OS sleep rounding lowers achieved Hz even when service is fast.
                # Calibrate against actual work, never against sleep precision.
                sustainable = pilot["service_p99_ns"] <= period
                if pilot["clean"] and sustainable:
                    final = trial(args, mode, size, slots, period, args.samples,
                                  f"{mode}-{size}MiB-measured-{attempt}")
                    attempts.append(final)
                    write_csv(args.output / "attempts.csv", attempts)
                    if final["integrity_errors"]:
                        raise RuntimeError("integrity failure in measured trial")
                    if final["clean"]:
                        results.append(final)
                        report(args, results)
                        print(f"  measured P99={final['latency_p99_ms']:.3f} ms; "
                              f"{final['receive_mib_s']:.2f} MiB/s", flush=True)
                        break
                    pilot = final
                if not pilot["clean"] and slots < maximum:
                    slots = min(slots * 2, maximum)
                else:
                    # Slow the offered load when capacity is exhausted or the
                    # sender cannot meet the requested schedule. Never hide it.
                    needed = math.ceil(pilot["service_p99_ns"] * 1.25)
                    period = max(period * 2, needed)
                    if period > MAX_PERIOD_NS:
                        raise RuntimeError("no clean sustainable rate within the bounded calibration policy")
            else:
                raise RuntimeError("calibration attempt budget exhausted; inspect attempts.csv")
    print(f"Results: {args.output / 'REPORT.md'}", flush=True)


if __name__ == "__main__":
    main()

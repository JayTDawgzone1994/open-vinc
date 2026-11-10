"""Run bounded, isolated pump cost cases against an installed-SDK App."""
import argparse
import csv
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import signal
import statistics
import subprocess

CASE_TIMEOUT_SECONDS = 65
TOPIC_COUNTS = (1, 4)
WORKER_COUNTS = (0, 1, 2, 4)
CPU_ITERATIONS = (0, 50000)
SAMPLES_PER_TOPIC = 2048


def tcl_word(value):
    # Paths are data, not Tcl substitutions or commands.
    text = str(value).replace("\\", "/")
    return '"' + text.replace("$", "\\$").replace("[", "\\[").replace('"', '\\"') + '"'


def run_case(app, output, workers, topics, iterations, repeat):
    name = f"w{workers}-t{topics}-i{iterations}-r{repeat}"
    result = output / f"{name}.csv"
    script = output / f"{name}.tcl"
    script.write_text(
        f"set result {tcl_word(result)}\n"
        "if {[catch {\n"
        f"  PumpCost {workers} {topics} {iterations} {SAMPLES_PER_TOPIC} $result\n"
        "  set deadline [expr {[clock milliseconds] + 45000}]\n"
        "  while {![file exists $result]} {\n"
        "    if {[clock milliseconds] > $deadline} { error {cost case timed out} }\n"
        "    after 10\n"
        "  }\n"
        "} diagnostic]} { puts \"PUMP_COST_FAILED: $diagnostic\" }\nexit\n",
        encoding="utf-8")
    with (output / f"{name}.log").open("wb") as log:
        child = subprocess.Popen([str(app), "--shell-port", "0", "-S", str(script)],
                                 cwd=output, stdin=subprocess.DEVNULL, stdout=log,
                                 stderr=subprocess.STDOUT, start_new_session=os.name != "nt")
        try:
            return_code = child.wait(timeout=CASE_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"],
                               check=False, timeout=15, stdout=log, stderr=subprocess.STDOUT)
            else:
                os.killpg(child.pid, signal.SIGKILL)
            child.wait(timeout=15)
            raise RuntimeError(f"{name}: process deadline exceeded")
    diagnostic = (output / f"{name}.log").read_text(encoding="utf-8", errors="replace")
    if return_code or "PUMP_COST_FAILED" in diagnostic or "All module processes exited" not in diagnostic:
        raise RuntimeError(f"{name}: failed; inspect its log")
    with result.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        raise RuntimeError(f"{name}: missing measurement")
    row = rows[0]
    if int(row["delivered"]) != topics * SAMPLES_PER_TOPIC or int(row["lost"]) != 0:
        raise RuntimeError(f"{name}: delivery failure")
    row["repeat"] = repeat
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--environment", required=True, help="Compiler, standard library and build configuration")
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10:
        parser.error("repeats must be between 1 and 10")
    app = args.app.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    metadata = {"platform": platform.platform(), "machine": platform.machine(),
                "logical_cpus": os.cpu_count(), "environment": args.environment,
                "app": str(app), "app_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
                "repeats": args.repeats, "samples_per_topic": SAMPLES_PER_TOPIC,
                "shuffle_seed": 20261110}
    metadata["module_sha256"] = {
        image.name: hashlib.sha256(image.read_bytes()).hexdigest()
        for image in (app.parent / "modules/native/PumpCost").iterdir()
        if image.suffix in (".dll", ".so", ".dylib")
    }
    (output / "environment.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    cases = list(itertools.product(WORKER_COUNTS, TOPIC_COUNTS, CPU_ITERATIONS, range(args.repeats)))
    random.Random(metadata["shuffle_seed"]).shuffle(cases)
    rows = []
    for workers, topics, iterations, repeat in cases:
        row = run_case(app, output, workers, topics, iterations, repeat)
        rows.append(row)
        print(f"{len(rows)}/{len(cases)}: workers={workers} topics={topics} iterations={iterations} "
              f"throughput={row['throughput_per_s']} p99_us={row['p99_us']}", flush=True)
    with (output / "all.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    keys = ("workers", "topics", "iterations")
    columns = [key for key in rows[0] if key not in keys + ("repeat", "checksum")]
    with (output / "median.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(keys) + columns)
        writer.writeheader()
        for workers, topics, iterations in itertools.product(WORKER_COUNTS, TOPIC_COUNTS, CPU_ITERATIONS):
            group = [row for row in rows if tuple(int(row[key]) for key in keys) == (workers, topics, iterations)]
            summary = dict(zip(keys, (workers, topics, iterations)))
            summary.update({key: statistics.median(float(row[key]) for row in group) for key in columns})
            writer.writerow(summary)


if __name__ == "__main__":
    main()

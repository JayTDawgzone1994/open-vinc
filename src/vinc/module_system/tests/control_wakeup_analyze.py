"""Analyze one isolated wakeup run; subtract timestamps only within a process."""

import argparse
import csv
import json
import math
from pathlib import Path


def read_trace(path):
    with path.open(encoding="utf-8", newline="") as source:
        return [{key: int(value) for key, value in row.items()} for row in csv.DictReader(source)]


def identity(row):
    return row["generation"], row["domain"], row["operation"], row["request"]


def index(rows, point, role, kind):
    selected = [row for row in rows if (row["point"], row["role"], row["kind"]) == (point, role, kind)]
    result = {identity(row): row["nanoseconds"] for row in selected}
    if len(result) != len(selected):
        raise ValueError("Duplicate frame observation; cannot silently pair requests")
    return result


def distribution(values):
    ordered = sorted(values)
    return {
        "count": len(ordered),
        "min_ms": ordered[0],
        "p50_ms": ordered[math.ceil(len(ordered) * 0.50) - 1],
        "p95_ms": ordered[math.ceil(len(ordered) * 0.95) - 1],
        "p99_ms": ordered[math.ceil(len(ordered) * 0.99) - 1],
        "max_ms": ordered[-1],
    }


def analyze(directory):
    parent = read_trace(directory / "parent.csv")
    child = read_trace(directory / "child.csv")
    # Point: Sent=0, Queued=1, Taken=2. Role: Central=1, Module=2.
    # Kind: Request=3, Response=4. These numbers belong to this trace format.
    queued = index(parent, 1, 1, 3)
    taken = index(parent, 2, 1, 3)
    replied = index(parent, 0, 1, 4)
    sent = index(child, 0, 2, 3)
    response_queued = index(child, 1, 2, 4)
    response_taken = index(child, 2, 2, 4)
    result = {
        "units": "milliseconds",
        "measurement": "application queue checkpoints, not physical wire timestamps",
        "clock_rule": "durations use one process clock; no cross-process epoch subtraction",
        "operations": {},
    }
    for name, operation in (("Subscribe", 0x0602), ("EnableNotifications", 0x060A)):
        keys = sorted(
            (key for key in queued if key[1:3] == (3, operation)),
            key=queued.get,
        )
        expected = 65 if name == "Subscribe" else 64
        if len(keys) != expected:
            raise ValueError(f"Expected {expected} {name} requests, got {len(keys)}")
        # The ordinary helper first creates one non-callback subscription.
        keys = keys[-64:]
        metrics = {name: [] for name in (
            "central_inbox_to_take", "central_take_to_reply_enqueue",
            "child_request_enqueue_to_reply_take", "child_reply_inbox_to_take",
            "transport_and_child_dispatch_remainder",
        )}
        for key in keys:
            inbox = taken[key] - queued[key]
            service = replied[key] - taken[key]
            round_trip = response_taken[key] - sent[key]
            child_dispatch = response_taken[key] - response_queued[key]
            durations = (inbox, service, round_trip, child_dispatch,
                         round_trip - inbox - service)
            if any(value < 0 for value in durations):
                raise ValueError(f"Invalid timing order for {key}")
            for metric, value in zip(metrics.values(), durations):
                metric.append(value / 1_000_000)
        result["operations"][name] = {
            metric: distribution(values) for metric, values in metrics.items()
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = analyze(args.directory)
    text = json.dumps(result, indent=2) + "\n"
    (args.directory / "summary.json").write_text(text, encoding="utf-8")
    print(text, end="")


if __name__ == "__main__":
    main()

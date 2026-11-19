"""Run matched opt-in Release cost targets, after other builds/tests finish."""
import argparse
import csv
import hashlib
import json
import os
import platform
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from control_wakeup_analyze import distribution, index, read_trace


def analyze(directory, count):
    parent = read_trace(directory / 'parent.csv')
    child = read_trace(directory / 'child.csv')
    queued = index(parent, 1, 1, 3)
    taken = index(parent, 2, 1, 3)
    sent = index(child, 0, 2, 3)
    replied = index(child, 2, 2, 4)
    result = {}
    for name, operation in (('Subscribe', 0x0602), ('EnableNotifications', 0x060A)):
        keys = sorted((key for key in queued if key[1:3] == (3, operation)), key=queued.get)
        if len(keys) != 2 * count:
            raise ValueError(f'{name}: expected {2 * count}, got {len(keys)}')
        for phase, selected in (('startup', keys[:count]), ('commands', keys[count:])):
            inbox = [(taken[key] - queued[key]) / 1e6 for key in selected]
            round_trip = [(replied[key] - sent[key]) / 1e6 for key in selected]
            if min(inbox + round_trip) < 0:
                raise ValueError('negative same-process duration')
            result[f'{phase}.{name}.central_queue'] = distribution(inbox)
            result[f'{phase}.{name}.round_trip'] = distribution(round_trip)
    with (directory / 'phases.csv').open(encoding='utf-8', newline='') as source:
        result['phases'] = [
            {key: value if key == 'phase' else float(value) for key, value in row.items()}
            for row in csv.DictReader(source)
        ]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binaries', type=Path, help='Module System test binary directory')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.binaries = args.binaries.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    suffix = '.exe' if os.name == 'nt' else ''
    plugin = 'module_control_cost_plugin.dll' if os.name == 'nt' else 'libmodule_control_cost_plugin.so'
    artifacts = [args.binaries / (f'module_control_cost_{policy}' + suffix)
                 for policy in ('polling', 'notified')]
    artifacts += [args.binaries / ('module_control_wakeup_host' + suffix), args.binaries / plugin]
    result = {
        'platform': platform.platform(),
        'python': sys.version,
        'binary_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in artifacts},
        'scope': 'central process CPU/resources; identical trace hooks; not payload throughput',
        'baseline': 'current sources with reconstructed pre-stage3 polling policy only',
        'runs': [],
    }
    for repeat in range(3):
        for count in (4, 128):
            policies = ('polling', 'notified') if repeat % 2 == 0 else ('notified', 'polling')
            for policy in policies:
                directory = args.output / f'{repeat}-{count}-{policy}'
                directory.mkdir()
                command = [str(args.binaries / (f'module_control_cost_{policy}' + suffix)),
                           str(artifacts[2]), str(artifacts[3]), str(count), str(directory.resolve())]
                with (directory / 'run.log').open('w', encoding='utf-8') as output:
                    completed = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=120)
                if completed.returncode:
                    raise RuntimeError(f'cost run failed: {directory}')
                result['runs'].append({'policy': policy, 'count': count, 'repeat': repeat,
                                       'metrics': analyze(directory, count)})
                (args.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
                print(directory.name, 'PASS', flush=True)


if __name__ == '__main__':
    main()

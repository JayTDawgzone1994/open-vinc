"""Reconstruct only the pre-stage-three supervisor policy in a build-tree copy.

Fail on changed source anchors instead of silently measuring the wrong policy.
All providers, protocols, workload and toolchain stay identical to the new build.
This is an opt-in test input, never an installed/runtime configuration switch.
"""
import argparse
from pathlib import Path


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Baseline source anchor changed: {old[:80]}")
    return text.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    source = replace_once(source,
        "std::shared_ptr<control::detail::ReceiveWake> wake =\n"
        "        std::make_shared<control::detail::ReceiveWake>();",
        "std::condition_variable wake;")
    source = replace_once(source,
        "                control::detail::ReceiveWakeAccess::bind(*record.session, wake);\n", "")
    if source.count("impl_->wake->notify();") != 5:
        raise ValueError("Management wake sites changed; review baseline")
    source = source.replace("impl_->wake->notify();", "impl_->wake.notify_all();")
    begin = source.index("    void monitor(std::stop_token token) {")
    end = source.index("    // Allow one outstanding command/completion per Module.", begin)
    old_monitor = Path(__file__).with_name("polling_monitor.inc").read_text(encoding="utf-8")
    source = source[:begin] + old_monitor + "\n" + source[end:]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    main()

"""Installed C++ Apps: port conflict, bounded recovery and shutdown isolation."""

import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time

from multi_app import run, require
from multi_app_runtime import RunningApp, graph, POLL_SECONDS, WAIT_SECONDS


ROOT = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location("services", ROOT / "apps/vinc/tests/system_services.py")
services = importlib.util.module_from_spec(spec)
spec.loader.exec_module(services)
LIMIT_OBSERVATION_SECONDS = 20
BUILD_JOBS = 4


def verify(first, second, work):
    first_app = RunningApp(first, sys.executable, work / "first.log")
    second_app = None
    try:
        first_port = first_app.connect()
        first_app.wait_log("VINC ready")
        first_ui = services.wait_running(first_app, "ui")
        first_recorder = services.wait_running(first_app, "recorder")
        first_graph = graph(31411)
        require(first_ui[1] in {int(row["mid"]) for row in first_graph["modules"]}, "First graph identity")
        duplicate = run([str(first), "--shell-port", "0"], success=False, timeout=WAIT_SECONDS)
        require("A Vinc with this name is already running" in duplicate, duplicate)

        second_app = RunningApp(second, sys.executable, work / "second.log")
        second_port = second_app.connect()
        second_app.wait_log("VINC ready")
        require(first_port != second_port, "Independent Shell ports")
        second_recorder = services.wait_running(second_app, "recorder")
        services.command_output(second_app, "recorder status", "bag: idle")
        require(first_recorder[2] != second_recorder[2], "Apps share a Recorder process")

        # First owns HTTP throughout this window. Real bind failures must expose
        # the rolling limit rather than restarting at an unbounded rate.
        deadline = time.monotonic() + LIMIT_OBSERVATION_SECONDS
        while time.monotonic() < deadline:
            limited = services.snapshot(second_app, "ui")
            if limited[0] == "RestartWaiting" and "rate_limited=1" in limited[3]:
                break
            time.sleep(POLL_SECONDS)
        else:
            raise RuntimeError(f"UI did not expose the start limit: {limited}")
        details = services.expect_reply(second_app, "help --verbose", "[System Modules]")
        require("BindFailed" in details, details)
        services.expect_reply(second_app, "ui status", "RestartWaiting")

        services.expect_reply(first_app, "ui disable", "UI disable requested")
        require(services.snapshot(first_app, "ui")[:3] == first_ui[:3], "Disable terminated First UI")
        recovered = services.wait_running(second_app, "ui", limited[1])
        require(recovered[1] > limited[1], "Recovery reused MID")
        second_graph = graph(31411)
        # MIDs are App-local. The recovered UI now has a distinct MID from A's
        # initial UI, so this observation can detect accidentally querying A.
        ui_mids = {int(row["mid"]) for row in second_graph["modules"] if row["name"] == "ui"}
        require(ui_mids == {recovered[1]} and recovered[1] != first_ui[1], "Graph crossed App ownership")

        # Recorder A continues across B's shutdown; normal A shutdown finishes
        # its own database. No Python Module is launched in either application.
        bag = work / "first-recording"
        first_app.command(f'recorder record "{bag.as_posix()}" --all')
        services.command_output(first_app, "recorder status", "bag: accepting=1")
        services.command_output(second_app, "recorder status", "bag: idle")
        second_app.stop()
        require(second_app.process.returncode == 0, "Second shutdown failed")
        require(services.snapshot(first_app, "ui")[:3] == first_ui[:3], "Second shutdown affected First")
        services.command_output(first_app, "recorder status", "bag: accepting=1")
        first_app.stop()
        require(first_app.process.returncode == 0 and services.recording_completed(bag),
                "First shutdown did not finish its recording")
        for app in (first_app, second_app):
            require("All module processes exited" in app.log.read_text(encoding="utf-8"), "Missing cleanup proof")
        print("PASS: moved C++ Apps, same-name exclusion, isolated services/graphs, "
              "real bind-failure rate limit and recovery, recording and shutdown isolation", flush=True)
    finally:
        try:
            if second_app is not None:
                second_app.stop()
        finally:
            first_app.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk-build", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--make-program", required=True)
    parser.add_argument("--config", required=True)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.work.resolve()))
    print(f"Artifacts: {work}", flush=True)
    sdk = work / "sdk"
    run([args.cmake, "--install", str(args.sdk_build), "--prefix", str(sdk), "--component", "VincSDK"])
    source = work / "source"
    shutil.copytree(Path(__file__).with_suffix(""), source)
    build = work / "build"
    run([args.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
         f"-DCMAKE_CXX_COMPILER={args.compiler}", f"-DCMAKE_MAKE_PROGRAM={args.make_program}",
         f"-DCMAKE_BUILD_TYPE={args.config}", f"-Dvinc_DIR={(sdk / 'lib/cmake/vinc').as_posix()}"])
    run([args.cmake, "--build", str(build), "--target", "SystemA", "SystemB", "--parallel", str(BUILD_JOBS)])
    print("Built two C++ Apps; no Python binding", flush=True)
    package = work / "package"
    for name in ("SystemA", "SystemB"):
        run([args.cmake, "--install", str(build), "--prefix", str(package), "--component", f"VincApp_{name}"])
    moved = work / "moved"
    package.rename(moved)
    suffix = ".exe" if os.name == "nt" else ""
    verify(moved / "SystemA" / ("SystemA" + suffix), moved / "SystemB" / ("SystemB" + suffix), work)


if __name__ == "__main__":
    main()

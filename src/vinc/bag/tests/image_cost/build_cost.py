"""Measure isolated Recorder consumers, sequentially, using one prebuilt Release SDK."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time


BUILD_TIMEOUT_SECONDS = 600
REPETITIONS = 3


def run(command, cwd, log, compiler=False):
    start = time.perf_counter()
    statistics = {}
    if compiler and os.name != "nt":
        command = ["/usr/bin/time", "-v", "-o", str(log.with_suffix(".time")), *shlex.split(command)]
    with log.open("wb") as output:
        process = subprocess.Popen(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=os.name != "nt")
        try:
            code = process.wait(timeout=BUILD_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            # Own this compiler/build tree only, including a compiler child of CMake.
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                               capture_output=True, timeout=10)
            else:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise
    statistics["seconds"] = time.perf_counter() - start
    statistics["exit_code"] = code
    if compiler and os.name == "nt":
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in (
                    "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                    "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                    "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]

        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(int(process._handle), ctypes.byref(counters), counters.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        statistics["peak_rss_bytes"] = counters.PeakWorkingSetSize
        statistics["peak_commit_bytes"] = counters.PeakPagefileUsage
    elif compiler:
        report = log.with_suffix(".time").read_text()
        statistics["peak_rss_bytes"] = int(re.search(r"Maximum resident set size \(kbytes\): (\d+)", report)[1]) * 1024
    if code != 0:
        raise RuntimeError(f"Command failed; see {log}")
    return statistics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("work", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--msvc-state", type=Path)
    args = parser.parse_args()
    if os.name != "nt" and sys.platform != "linux":
        parser.error("This cost sampler needs a native macOS implementation; do not substitute Linux RSS data.")
    if args.msvc_state:
        state = json.loads(args.msvc_state.read_text("utf-8"))["toolchain"]["msvc_environment"]
        os.environ.update(state)
        sdk_bin = Path(os.environ["ProgramFiles(x86)"]) / "Windows Kits/10/bin" / state["WINDOWSSDKVERSION"].strip("\\/") / "x64"
        os.environ["PATH"] = os.pathsep.join([str(Path(args.compiler).parent), str(sdk_bin), os.environ["PATH"]])
    work = args.work.resolve()
    report = {"compiler": args.compiler, "sdk": str(args.sdk.resolve()), "jobs": 1, "variants": {}}
    recipes = {}
    for variant in ("typed", "generic"):
        source = work / variant
        tree = work / (variant + "-build")
        configure = ["cmake", "-S", str(source), "-B", str(tree), "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=" + args.compiler,
                     "-DCMAKE_MAKE_PROGRAM=" + args.ninja,
                     "-Dvinc_DIR=" + str(args.sdk.resolve() / "lib/cmake/vinc")]
        run(configure, work, work / (variant + "-configure.log"))
        (work / (variant + "-commands.txt")).write_bytes(
            subprocess.check_output([args.ninja, "-t", "commands", "CostApp"], cwd=tree))
        full = run(["cmake", "--build", str(tree), "--target", "CostApp", "--parallel", "1"],
                   work, work / (variant + "-full.log"))
        commands = json.loads(subprocess.check_output([args.ninja, "-t", "compdb"], cwd=tree))
        recipe = next(item for item in commands if "cost_recorder.dir" in item["output"]
                      and item["file"].endswith("recorder_module.cpp"))
        recipes[variant] = recipe
        deployed = tree / "output/CostApp/Release"
        files = {str(path.relative_to(deployed)): path.stat().st_size for path in deployed.rglob("*") if path.is_file()}
        report["variants"][variant] = {"clean_consumer_build": full, "deployed_files_bytes": files,
                                       "compile": []}
        (work / "build-results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    # Alternating direct compiler runs reduce order effects; neither touches production objects.
    for repetition in range(REPETITIONS):
        for variant in (("typed", "generic") if repetition % 2 == 0 else ("generic", "typed")):
            recipe = recipes[variant]
            output = work / f"{variant}-{repetition}.obj"
            if os.name == "nt":
                command = re.sub(r"/Fo\S+", '/Fo"' + output.as_posix() + '"', recipe["command"])
                command = re.sub(r"/Fd\S+", '/Fd"' + output.with_suffix(".pdb").as_posix() + '"', command)
            else:
                tokens = shlex.split(recipe["command"])
                tokens[tokens.index("-o") + 1] = str(output)
                command = shlex.join(tokens)
            measured = run(command, work / (variant + "-build"),
                           work / f"{variant}-{repetition}.log", compiler=True)
            measured["object_bytes"] = output.stat().st_size
            report["variants"][variant]["compile"].append(measured)
            (work / "compile-recipes.json").write_text(json.dumps(recipes, indent=2), encoding="utf-8")
            (work / "build-results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    for variant in ("typed", "generic"):
        # A comment-only message edit exercises generator/dependency decisions without ABI changes.
        message = work / variant / "messages/CostUnused0.msg"
        with message.open("a", encoding="utf-8") as stream:
            stream.write("\n// Incremental generation check.\n")
        report["variants"][variant]["incremental_message_edit"] = run(
            ["cmake", "--build", str(work / (variant + "-build")), "--target", "CostApp", "--parallel", "1"],
            work, work / (variant + "-incremental.log"))
    (work / "compile-recipes.json").write_text(json.dumps(recipes, indent=2), encoding="utf-8")
    (work / "build-results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("BUILD_COST_OK", flush=True)


if __name__ == "__main__":
    main()

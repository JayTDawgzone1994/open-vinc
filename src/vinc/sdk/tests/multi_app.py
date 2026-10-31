"""Exercise per-App composition, real deployment and official startup isolation."""

import argparse
import hashlib
import os
from pathlib import Path
import signal
import socket
import shutil
import subprocess
import sys
import tempfile
from multi_app_runtime import verify_coexistence


BUILD_TIMEOUT_SECONDS = 900
APP_TIMEOUT_SECONDS = 30
STARTUP_FAILURE_TIMEOUT_SECONDS = 60
CLEANUP_TIMEOUT_SECONDS = 10
BUILD_JOBS = 2


def run(command, *, success=True, timeout=BUILD_TIMEOUT_SECONDS, env=None):
    # A timed-out App may own child Modules. Kill only this test's process tree,
    # not every matching executable or unrelated central on the machine.
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, encoding="utf-8", errors="replace",
                          start_new_session=os.name != "nt", env=env) as process:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                               capture_output=True, timeout=CLEANUP_TIMEOUT_SECONDS)
            else:
                os.killpg(process.pid, signal.SIGKILL)
            process.kill()
            process.communicate(timeout=CLEANUP_TIMEOUT_SECONDS)
            raise
    output = stdout + stderr
    if (process.returncode == 0) != success:
        raise RuntimeError(f"Command failed: {command}\n{output}")
    return output


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--generator", default="Ninja", choices=("Ninja",))
    parser.add_argument("--make-program", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--python", required=True)
    parser.add_argument("--work", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--sdk-build", type=Path)
    mode.add_argument("--source", type=Path)
    args = parser.parse_args()
    # Each invocation needs a fresh binary tree to prove target exclusion, while
    # retaining failed artifacts for diagnosis rather than deleting user files.
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.work.resolve()))
    print(f"Artifacts: {work}", flush=True)
    source = work / "source"
    build = work / "build"
    fixture = Path(__file__).with_name("multi_app")
    shutil.copytree(fixture, source, dirs_exist_ok=True)
    configure = [args.cmake, "-S", str(source), "-B", str(build), "-G", args.generator,
                 f"-DCMAKE_CXX_COMPILER={args.compiler}", f"-DCMAKE_MAKE_PROGRAM={args.make_program}",
                 f"-DCMAKE_BUILD_TYPE={args.config}", f"-DPython_EXECUTABLE={args.python}",
                 f"-DPython3_EXECUTABLE={sys.executable}"]
    if args.sdk_build:
        sdk = work / "sdk"
        run([args.cmake, "--install", str(args.sdk_build), "--component", "VincSDK", "--prefix", str(sdk)])
        configure.append(f"-Dvinc_DIR={(sdk / 'lib/cmake/vinc').as_posix()}")
        deployment_script = sdk / "lib/cmake/vinc/DeployApplication.cmake"
    else:
        configure.append(f"-DVINC_SOURCE={args.source.resolve().as_posix()}")
        deployment_script = args.source / "src/vinc/sdk/cmake/DeployApplication.cmake"
    run(configure)
    print("Configured three Apps with late Module/message registration", flush=True)
    build_command = [args.cmake, "--build", str(build), "--config", args.config,
                     "--parallel", str(BUILD_JOBS)]

    def recorded(name, extension="path"):
        return (build / f"{name}-{args.config}.{extension}").read_text(encoding="utf-8").strip()

    run(build_command + ["--target", "First"])
    print("Built First without Second's Module or Python binding", flush=True)
    require(not Path(recorded("SecondModule")).exists(), "First built Second's Module")
    require(not Path(recorded("native")).exists(), "Python-disabled App built the native binding")
    shared = Path(recorded("SharedModule"))
    # Ribosome intentionally regenerates its entire output on every build.
    # Prove one shared compile within one multi-target build, not unchanged
    # mtimes across separate invocations of that existing generation policy.
    output = run(build_command + ["--target", "Second", "Third", "StalledOfficial"])
    normalized = output.replace("\\", "/")
    require(normalized.count("Building CXX object CMakeFiles/SharedModule.dir/module.cpp") == 1,
            f"Shared Module was not compiled once in the combined build:\n{output}")
    print("Built Second/Third together with one shared Module and Python binding", flush=True)
    native = Path(recorded("native"))
    require(shared.is_file() and native.is_file(), "Shared build artifacts are missing")
    require(recorded("First", "python").endswith("-NOTFOUND"), "First acquired Python dependency")
    require(recorded("Second", "python") == recorded("Third", "python") == "vinc_python_native",
            "Python Apps did not share one binding target")

    for app, expected, excluded in (("First", "FirstModule", "SecondModule"),
                                    ("Second", "SecondModule", "FirstModule"),
                                    ("Third", "SharedModule", "FirstModule")):
        generated = Path(recorded(app, "generated"))
        catalog = (generated / "module_catalog.hpp").read_text(encoding="utf-8")
        require(f'"{expected}"' in catalog and f'"{excluded}"' not in catalog, "Catalog isolation failed")
        require('"webUi"' in catalog and '"bagRecorder"' in catalog, "Official Modules missing")
        require(f"modules/SharedModule/{shared.name}" in catalog, "Shared Plugin is missing")
        require(shared.as_posix() not in catalog, "Catalog retains build-tree paths")
        require(f"{app}_vinc_ui" in recorded(app, "selection"), "App-local UI missing")
        ui_header = (generated / "vinc_ui_paths.hpp").read_text(encoding="utf-8")
        require(f"vinc_data/multi-{app.lower()}/ui" in ui_header, "UI root borrowed another identity")
        executable = Path(recorded(app))
        destination = executable.parent
        if app != "First":
            module = destination / "data/module/ScriptPeer/ScriptPeer.py"
            module.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / "ScriptPeer.py", module)
        output = run([str(executable), "--shell-port", "0", "--python", args.python], timeout=APP_TIMEOUT_SECONDS)
        require(f"{app}_SCRIPT_OK" in output and "APP_TEST_FAILED" not in output, output)
        require(f"{expected} SELF_HOST_OK 42" in output, output)
        require("All module processes exited" in output, output)
        if app != "First":
            require("PYTHON_MODULE_OK" in output, output)
        print(f"PASS: {app} catalog/script/identity and real Host execution", flush=True)

    # Install exactly one App, then a second into the same prefix. Live data is
    # not an installation input, and sibling App ownership stays separate.
    package = work / "package"
    run([args.cmake, "--install", str(build), "--prefix", str(package), "--component", "VincApp_First"])
    require((package / "First").is_dir() and not (package / "Second").exists(), "Individual install failed")
    run([args.cmake, "--install", str(build), "--prefix", str(package), "--component", "VincApp_Second"])
    moved = work / "moved-package"
    package.rename(moved)
    first = moved / "First" / Path(recorded("First")).name
    output = run([str(first), "--shell-port", "0"], timeout=APP_TIMEOUT_SECONDS)
    require("First_SCRIPT_OK" in output and "APP_TEST_FAILED" not in output, output)
    require("bag: idle" in output and "Official Module webUi ready" in output, output)
    output = run([str(first), "--shell-port", "0"],
                 env=dict(os.environ, VINC_TEST_STOP_UI="1"), timeout=APP_TIMEOUT_SECONDS)
    require("First_SCRIPT_OK" in output and "APP_TEST_FAILED" not in output, output)
    require(not (moved / "Second/data/module/ScriptPeer").exists(), "Install copied user Python Modules")
    peer = moved / "Second/data/module/ScriptPeer/ScriptPeer.py"
    peer.parent.mkdir(parents=True)
    shutil.copy2(source / "ScriptPeer.py", peer)
    second = moved / "Second" / Path(recorded("Second")).name
    output = run([str(second), "--shell-port", "0", "--python", args.python], timeout=APP_TIMEOUT_SECONDS)
    require("Second_SCRIPT_OK" in output and "PYTHON_MODULE_OK" in output and "APP_TEST_FAILED" not in output, output)
    verify_coexistence(first, second, args.python, work, run, require)

    # Bind failure is local to UI. Bag is ready and the script still completes.
    with socket.socket() as occupied:
        if os.name != "nt":
            # Prior HTTP checks leave TIME_WAIT sockets, not live listeners.
            # POSIX reuse permits rebinding those while still rejecting a listener.
            occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        occupied.bind(("127.0.0.1", 8080))
        occupied.listen()
        environment = dict(os.environ, VINC_TEST_UI_UNAVAILABLE="1")
        output = run([str(first), "--shell-port", "0"], env=environment, timeout=APP_TIMEOUT_SECONDS)
    require("Official Module webUi MID=" in output and "failed:" in output, output)
    require("First_SCRIPT_OK" in output and "bag: idle" in output and "APP_TEST_FAILED" not in output, output)

    # Missing official Plugin is also isolated, without disguising command failure.
    recorder = next((first.parent / "modules/bagRecorder").iterdir())
    hidden = recorder.with_suffix(recorder.suffix + ".hidden")
    recorder.rename(hidden)
    try:
        environment = dict(os.environ, VINC_TEST_BAG_UNAVAILABLE="1")
        output = run([str(first), "--shell-port", "0"], env=environment, timeout=APP_TIMEOUT_SECONDS)
        require("Official Module bagRecorder MID=" in output and "First_SCRIPT_OK" in output
                and "APP_TEST_FAILED" not in output, output)
    finally:
        hidden.rename(recorder)
    print("PASS: isolated installation, relocation, UI bind and Bag load failure", flush=True)

    ui_plugin = next((first.parent / "modules/webUi").iterdir())
    backup = ui_plugin.with_suffix(ui_plugin.suffix + ".backup")
    ui_plugin.rename(backup)
    try:
        shutil.copy2(Path(recorded("StalledOfficial")), ui_plugin)
        output = run([str(first), "--shell-port", "0"],
                     env=dict(os.environ, VINC_TEST_UI_UNAVAILABLE="1"),
                     timeout=STARTUP_FAILURE_TIMEOUT_SECONDS)
        require("Official Module webUi MID=" in output and "failed:" in output, output)
        require("bag: idle" in output and "First_SCRIPT_OK" in output
                and "APP_TEST_FAILED" not in output and "All module processes exited" in output, output)
    finally:
        # The destination is our copied fixture, never an input Plugin artifact.
        backup.replace(ui_plugin)
    print("PASS: stalled official loader has bounded failure and later startup continues", flush=True)

    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    first_manifest = Path(recorded("First", "generated")) / "deployment.cmake"
    second_manifest = Path(recorded("Second", "generated")) / "deployment.cmake"
    first_owned = first.parent / ".vinc-owned-files"
    before = digest(first_owned)
    output = run([args.cmake, f"-DMANIFEST={second_manifest}", f"-DDEPLOY_TO={first.parent}",
                  "-P", str(deployment_script)], success=False)
    require("belongs to another App" in output and digest(first_owned) == before, output)
    for name, suffix, diagnostic in (
        ("duplicate", '\nlist(APPEND module_sources "${app_source}")\n'
         'list(APPEND module_destinations "${app_name}" "${app_name}")\n'
         'list(APPEND module_sources "${app_source}")\n', "paths collide"),
        ("traversal", '\nlist(APPEND module_sources "${app_source}")\n'
         'list(APPEND module_destinations "../escape")\n', "Invalid managed deployment path"),
    ):
        invalid = work / f"{name}.cmake"
        invalid.write_text(first_manifest.read_text(encoding="utf-8") + suffix, encoding="utf-8")
        output = run([args.cmake, f"-DMANIFEST={invalid}", f"-DDEPLOY_TO={first.parent}",
                      "-P", str(deployment_script)], success=False)
        require(diagnostic in output and digest(first_owned) == before, output)

    notice = second.parent / "licenses/open-vinc/NOTICE"
    notice.unlink()
    run([args.cmake, "--install", str(build), "--prefix", str(moved), "--component", "VincApp_Second"])
    require(notice.is_file(), "Deleted managed license was not repaired")
    modified = second.parent / "python/vinc/__init__.py"
    modified.write_text("# User modified former package file\n", encoding="utf-8")
    second_config = source / "second/app.config"
    second_config.write_text("CONFIG_VINC_MODULE_SecondModule=y\n"
                             "# CONFIG_VINC_APP_PYTHON is not set\n", encoding="utf-8")
    run(configure)
    run(build_command + ["--target", "Second"])
    run([args.cmake, "--install", str(build), "--prefix", str(moved), "--component", "VincApp_Second"])
    require(not (second.parent / "modules/SharedModule" / shared.name).exists(), "Stale Plugin retained")
    require(not (second.parent / "python/vinc" / native.name).exists(), "Stale binding retained")
    require(modified.read_text(encoding="utf-8").startswith("# User modified"), "User edit deleted")
    require(peer.is_file(), "User Python Module deleted")
    require(digest(first_owned) == before, "Updating Second changed First's manifest")
    require((first.parent / "modules/SharedModule" / shared.name).is_file(), "Sibling Plugin deleted")
    output = run([str(second), "--shell-port", "0"], timeout=APP_TIMEOUT_SECONDS)
    require("Second_SCRIPT_OK" in output and "APP_TEST_FAILED" not in output, output)
    print("PASS: ownership/path rejection, resource repair and isolated selection cleanup", flush=True)

    # Restore valid configuration after each negative check; no file deletion is needed.
    output = run(configure + ["-DRESERVED_TYPE=ON"], success=False)
    require("reserved for the official" in output, output)
    run(configure + ["-DRESERVED_TYPE=OFF"])
    print("PASS: shared Plugin/Python reuse, official UI isolation and reserved-name rejection", flush=True)


if __name__ == "__main__":
    main()

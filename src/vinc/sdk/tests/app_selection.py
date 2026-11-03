"""Build real tiny Plugins to validate App selection without constructing a Runtime."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


COMMAND_TIMEOUT_SECONDS = 120


def run(command, *, success=True):
    # Compiler diagnostics can mix localized native output and CMake UTF-8.
    # Replacement applies only to displayed diagnostics, never paths or inputs.
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                            timeout=COMMAND_TIMEOUT_SECONDS)
    output = result.stdout + result.stderr
    if (result.returncode == 0) != success:
        raise RuntimeError(f"Unexpected command result: {command}\n{output}")
    return output


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--make-program", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[4]
    fixture = Path(__file__).with_name("app_selection")
    args.work.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="selection space-", dir=args.work) as directory:
        root = Path(directory)
        source = root / "source"
        build = root / "build"
        shutil.copytree(fixture, source)
        configure = [args.cmake, "-S", str(source), "-B", str(build), "-G", args.generator,
                     f"-DCMAKE_CXX_COMPILER={args.compiler}",
                     f"-DCMAKE_MAKE_PROGRAM={args.make_program}",
                     f"-DCMAKE_BUILD_TYPE={args.config}", f"-DVINC_SOURCE={repository.as_posix()}",
                     f"-DPython3_EXECUTABLE={sys.executable}"]
        build_command = [args.cmake, "--build", str(build), "--config", args.config, "--parallel", "2"]
        run(configure)
        run(build_command + ["--target", "First"])
        plugins = build / "plugins" / args.config
        require((plugins / "camera.plugin").is_file(), "First did not build camera")
        require((plugins / "official.plugin").is_file(), "Official Module was not included")
        require(not (plugins / "detector.plugin").exists(), "First built Second's exclusive Module")
        shared_time = (plugins / "shared.plugin").stat().st_mtime_ns
        run(build_command)
        require((plugins / "detector.plugin").is_file(), "Default build omitted Second's Module")
        require((plugins / "shared.plugin").stat().st_mtime_ns == shared_time,
                "Shared Module was rebuilt for another App")
        require(not list(build.rglob("*forbidden.cpp.obj")), "Unselected source was compiled")
        catalog = (build / "selected_catalog.hpp").read_text()
        require('"camera"' in catalog and '"detector"' not in catalog,
                "Catalog rendering ignored the supplied selection")

        # Exercise the exact per-App arguments wired into both editor targets.
        # This checks file ownership/save/reconfigure, not graphical UI behavior.
        second_config = (source / "Second.config").read_bytes()
        second_result = (build / "vinc-config/Second/features.cmake").read_bytes()
        editor_arguments = (build / "First.editor-args").read_text().splitlines()
        run([sys.executable, "-B", str(repository / "tools/configure/configure.py"),
             *editor_arguments, "--save", "--set", "VINC_MODULE_camera=n",
             "--set", "VINC_MODULE_detector=y"])
        run(build_command + ["--target", "First"])
        selection = (build / "First.selected").read_text().split(";")
        require("camera" not in selection and "detector" in selection, "Config edit did not regenerate")
        require((source / "Second.config").read_bytes() == second_config, "Editor changed Second's config")
        require((build / "vinc-config/Second/features.cmake").read_bytes() == second_result,
                "First's configuration leaked into Second")

        # Required files cannot disappear silently; restoration must recover.
        first_config = source / "First.config"
        saved = first_config.read_bytes()
        first_config.unlink()
        output = run(build_command + ["--target", "First"], success=False)
        require("configuration file does not exist" in output, output)
        first_config.write_bytes(saved)
        run(build_command + ["--target", "First"])
        first_config.write_text("CONFIG_VINC_MODULE_Removed=y\n", encoding="utf-8")
        output = run(build_command + ["--target", "First"], success=False)
        require("unknown option" in output, output)
        first_config.write_bytes(saved)
        output = run(configure + ["-DDUPLICATE_TYPE=ON"], success=False)
        require("Duplicate ModuleType" in output, output)
        run(configure + ["-DDUPLICATE_TYPE=OFF"])
        run(build_command)
        print("PASS: selection, default exclusion, shared reuse, late registration, target customization,")
        print("      catalog boundary, per-App save/reconfigure, missing input, unknown option, duplicate rejection")


if __name__ == "__main__":
    main()

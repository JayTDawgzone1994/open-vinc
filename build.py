#!/usr/bin/env python3
"""Build a native Ninja tree, or explicitly build, verify and install the Vinc SDK."""

import argparse
import ctypes
import importlib.util
import os
from pathlib import Path
import re
import runpy
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
PYTHON_POLICY = runpy.run_path(str(ROOT / "src/vinc/common/cmake/python_version.py"))

# Reject unsupported interpreters before loading the build implementation.
if __name__ == "__main__":
    python_error = PYTHON_POLICY["version_error"]()
    if python_error:
        print("Build failed: " + python_error, file=sys.stderr)
        sys.exit(1)


def load_helper(name):
    # Use this checkout's file, never a third-party Python package named tools.
    spec = importlib.util.spec_from_file_location("_vinc_" + name, ROOT / "tools" / (name + ".py"))
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    return helper


process = load_helper("build_process")
toolchain = load_helper("build_toolchain")
profiles = load_helper("toolchain_profile")
# The SDK installer shares these operations with the ordinary build entry.
display = process.display
build_lock = process.build_lock
run_step = process.run_step


def fail(message):
    raise RuntimeError(message)


def positive_integer(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be at least 1")

    return number


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", choices=("Debug", "Release"),
                        help="Debug for builds; Release for SDK installation")
    parser.add_argument("--install-vinc-sdk", action="store_true", help="build and install this checkout's SDK")
    parser.add_argument("--sdk-root", type=Path, help="dedicated SDK installation directory")
    parser.add_argument("--no-register", action="store_true", help="install without changing CMake's user registry")
    parser.add_argument("--source", type=Path, default=ROOT / "src",
                        help="directory containing CMakeLists.txt (default: Open-VINC/src)")
    parser.add_argument("--build-dir", type=Path, help="override the per-platform/config build directory")
    parser.add_argument("--target", nargs="+", help="CMake targets; omitted means the default build")
    parser.add_argument("--jobs", type=positive_integer, help="parallel jobs; omitted means Ninja's default")
    parser.add_argument("-D", dest="definitions", action="append", default=[], metavar="NAME=VALUE",
                        help="CMake cache definition, repeatable; quote values containing spaces")
    args = parser.parse_args(argv)
    args.config = args.config or ("Release" if args.install_vinc_sdk else "Debug")
    args.source = args.source.resolve()
    if not args.install_vinc_sdk and (args.sdk_root is not None or args.no_register):
        parser.error("--sdk-root and --no-register require --install-vinc-sdk")

    if args.install_vinc_sdk and (args.source != ROOT / "src" or args.target):
        parser.error("SDK installation builds this checkout in full; external --source/--target are unsupported")

    if not (args.source / "CMakeLists.txt").is_file():
        parser.error("--source must contain CMakeLists.txt: " + str(args.source))

    if args.target and any(target.startswith("-") for target in args.target):
        parser.error("target names must not start with '-'")

    # These fields are owned by this native, single-config wrapper. In particular,
    # a toolchain file could override compiler selection after the cache check.
    reserved = {"CMAKE_BUILD_TYPE", "CMAKE_CONFIGURATION_TYPES", "CMAKE_TOOLCHAIN_FILE",
                "CMAKE_HOME_DIRECTORY", "CMAKE_CACHEFILE_DIR", "CMAKE_MAKE_PROGRAM",
                "CMAKE_SYSTEM_NAME", "CMAKE_SYSTEM_PROCESSOR", "CMAKE_OSX_ARCHITECTURES"}
    if args.install_vinc_sdk:
        reserved.update({"VINC_INSTALL_SDK", "VINC_BUILD_APPS", "VINC_BUILD_EXAMPLES",
                         "VINC_BUILD_UI", "VINC_BUILD_PYTHON", "BUILD_TESTING",
                         "CMAKE_INSTALL_PREFIX"})
    args.cache_values = {}
    for definition in args.definitions:
        match = re.fullmatch(r"([A-Za-z_][A-Za-z_0-9]*)(?::[A-Za-z_]+)?=(.*)", definition, re.DOTALL)
        if not match:
            parser.error("expected -DNAME[:TYPE]=VALUE")

        name, value = match.groups()
        if name == "Python_EXECUTABLE":
            parser.error("Python_EXECUTABLE was removed; select Module Python in App Kconfig. "
                         "Python3_EXECUTABLE selects only the configuration tool.")
        if name in reserved or name.startswith("CMAKE_GENERATOR"):
            parser.error(name + " is controlled by this native Ninja wrapper; use CMake directly")

        if name in args.cache_values:
            parser.error("duplicate cache definition: " + name)

        args.cache_values[name] = value

    return args


def default_build_directory(args, identity):
    if os.name == "nt":
        profile = "windows-msvc"
    elif sys.platform == "darwin":
        profile = "macos-clang"
    else:
        family = "clang" if identity["CXX"]["id"] in ("Clang", "AppleClang") else "gcc"
        profile = "linux-" + family

    parent = ROOT if args.source == ROOT / "src" else args.source
    purpose = "-sdk" if args.install_vinc_sdk else ""
    return parent / "build" / (profile + purpose + "-" + args.config.lower())


def build(args, after_build=None, progress=None):
    if ctypes.sizeof(ctypes.c_void_p) != 8 or sys.platform not in ("win32", "linux", "darwin"):
        fail("Use 64-bit Python on Windows, Linux or macOS for native builds.")

    selection = profiles.load() if args.source == ROOT / "src" else {}
    args.toolchain_profile = selection
    explicit_native = os.name == "nt" and bool(os.environ.get("VSCMD_VER"))
    environment = toolchain.build_environment(selection)
    # A complete caller-provided developer environment is an explicit choice.
    # An environment reconstructed from the profile is not an override of it.
    if selection and not explicit_native and os.name == "nt":
        for variable, key, cmake_key in (("CC", "cc", "CMAKE_C_COMPILER"), ("CXX", "cxx", "CMAKE_CXX_COMPILER")):
            if not os.environ.get(variable) and cmake_key not in args.cache_values:
                # NMAKE-based dependencies also inspect CC. A Windows path with
                # spaces is not their command syntax; select it through CMake.
                args.cache_values[cmake_key] = profiles.require_file(selection[key])
    profiles.apply(args, environment, selection)
    if selection:
        display("Toolchain selections: " + str(profiles.profile_path()))
    with process.terminal_color(environment) as colors:
        return build_with_environment(args, environment, colors, after_build, progress)


def build_with_environment(args, environment, colors, after_build, progress):
    selection = getattr(args, "toolchain_profile", {})
    cmake = toolchain.executable(profiles.selected_tool("cmake", selection, environment), environment)
    version = toolchain.tool_output([cmake, "--version"], environment, ROOT)
    match = re.search(r"cmake version (\d+)\.(\d+)", version)
    if not match or tuple(map(int, match.groups())) < (3, 20):
        fail("CMake version is unsupported.\nRequired: CMake >= 3.20\nFound: "
             + version + "\nPath: " + cmake + "\nRun "
             + ("tools\\setup.bat" if os.name == "nt" else "bash tools/setup.sh")
             + " --recommended, or select VINC_BUILD_CMAKE explicitly.")

    ninja = toolchain.executable(profiles.selected_tool("ninja", selection, environment), environment)
    compilers = toolchain.selected_compilers(args, environment)
    identity = toolchain.toolchain_identity(compilers, environment)
    environment = process.color_environment(environment, colors)
    color_definition = compiler_color_definition(args, tuple(map(int, match.groups())), colors)
    directory = (args.build_dir or default_build_directory(args, identity)).resolve()
    if directory == args.source or directory in args.source.parents:
        fail("--build-dir must not be the source directory or one of its parents.")

    if os.name == "nt" and " " in str(directory):
        display("Note: source builds of Tcl cannot use a build root containing spaces. "
                "Installed SDK consumers do not compile Tcl.")

    directory.mkdir(parents=True, exist_ok=True)
    with build_lock(directory):
        toolchain.validate_cache(directory, args, identity, reporter=display)
        cache = toolchain.read_cache(directory)
        configure = [cmake, "-S", str(args.source), "-B", str(directory), "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=" + args.config, "-DCMAKE_MAKE_PROGRAM=" + ninja]
        if color_definition:
            configure.append(color_definition)
        for language, path in compilers.items():
            # Validation proved the same executable and driver role. Keep CMake's
            # spelling so an equivalent alias does not trigger its cache reset.
            compiler_key = "CMAKE_" + language + "_COMPILER"
            configure.append("-D" + compiler_key + "=" + cache.get(compiler_key, path))

        for definition in args.definitions:
            if definition.split("=", 1)[0].split(":", 1)[0] not in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER"):
                configure.append("-D" + definition)

        command = [cmake, "--build", str(directory)]
        if args.target:
            command.extend(["--target"] + args.target)

        if args.jobs:
            command.extend(["--parallel", str(args.jobs)])

        log_path = directory / ".vinc-build.log"
        display("Source: " + str(args.source) + "\nBuild: " + str(directory) + "\nLog: " + str(log_path))
        with log_path.open("wb") as handle:
            log = process.BuildLog(handle, color=colors)
            if progress is not None:
                progress("configure", log_path)

            code = run_step(configure, args.source, environment, log)
            if code:
                log.write("\nConfigure failed; build was not started.\n")
                return code

            # A source project's forced cache settings must not silently replace
            # the profile selected on this command line.
            toolchain.validate_cache(directory, args, identity, report_unmanaged=False, reporter=display)

            # Persist identity only after CMake accepted the toolchain. Reusing an
            # unmanaged cache checks its paths/config; its old compiler version is unknown.
            toolchain.save_toolchain_record(directory, args, identity)
            if progress is not None:
                progress("build", log_path)

            code = run_step(command, args.source, environment, log)
            log.write("\nBuild " + ("succeeded." if code == 0 else "failed.") + " Tests were not run.\n")
            if code == 0 and after_build is not None:
                # Installation must consume this exact build while its lock is held.
                return after_build(cmake, ninja, directory, identity, environment, log)

            return code


def compiler_color_definition(args, cmake_version, enabled):
    # Preserve explicit project/compiler choices; do not append or rewrite CXX_FLAGS.
    if "CMAKE_COLOR_DIAGNOSTICS" in args.cache_values:
        return None
    if cmake_version >= (3, 24):
        return "-DCMAKE_COLOR_DIAGNOSTICS=" + ("ON" if enabled else "OFF")
    if enabled:
        display("Note: automatic compiler colors require CMake 3.24+; "
                "older CMake keeps existing compiler flags. Tool colors are still forwarded.")
    return None


def main(argv=None):
    python_error = PYTHON_POLICY["version_error"]()
    if python_error:
        display("Build failed: " + python_error, file=sys.stderr)
        return 1

    args = arguments(argv)
    try:
        if args.install_vinc_sdk:
            sdk_install = load_helper("sdk_install")
            return sdk_install.run_install(args, sys.modules[__name__])

        return build(args)
    except KeyboardInterrupt:
        display("Build cancelled.", file=sys.stderr)
        return 130
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        display("Build error: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

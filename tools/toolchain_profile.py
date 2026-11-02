"""Local tool selections, separate from per-build compatibility fingerprints."""

import argparse
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MAX_PROFILE_BYTES = 16384
PATH_KEYS = ("build_python", "config_python", "module_python", "cc", "cxx",
             "cmake", "ninja", "node", "npm", "vsdevcmd", "developer_dir")
VERSION_KEYS = ("python_version", "compiler_version", "cmake_version", "ninja_version", "node_version")
KEYS = ("format", "platform", "arch", "toolset", "windows_sdk") + PATH_KEYS + VERSION_KEYS


def platform_key():
    name = {"win32": "windows", "darwin": "macos", "linux": "linux"}.get(sys.platform)
    if name is None:
        raise RuntimeError("Unsupported toolchain profile platform: " + sys.platform)
    if name == "linux" and os.environ.get("WSL_DISTRO_NAME"):
        name = "wsl-" + re.sub(r"[^A-Za-z0-9._-]", "_", os.environ["WSL_DISTRO_NAME"])
    return name


def architecture():
    name = platform.machine().lower()
    return {"amd64": "x86_64", "aarch64": "arm64"}.get(name, name)


def profile_path(root=ROOT):
    return Path(root) / ".vinc/toolchains" / (platform_key() + "-" + architecture() + ".conf")


def validate(values):
    if values.get("format") != "1" or values.get("platform") != platform_key() or values.get("arch") != architecture():
        raise ValueError("Profile format/platform/architecture does not match this environment")
    for key in ("build_python", "config_python", "cc", "cxx", "cmake", "ninja"):
        if not values.get(key):
            raise ValueError("Missing profile field: " + key)
    for key, value in values.items():
        if key not in KEYS or any(ord(character) < 32 for character in value):
            raise ValueError("Invalid profile field: " + key)
        if key in PATH_KEYS and value and not Path(value).is_absolute():
            raise ValueError("Profile paths must be absolute: " + key)
    for key in ("toolset", "windows_sdk"):
        if values.get(key) and not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", values[key]):
            raise ValueError("Invalid native version selector: " + key)


def load(root=ROOT):
    path = profile_path(root)
    if not path.exists():
        return {}
    try:
        if path.stat().st_size > MAX_PROFILE_BYTES:
            raise ValueError("Profile exceeds size limit")
        values = {}
        for line in path.read_text(encoding="utf-8").splitlines():
            key, separator, value = line.partition("=")
            if not separator or key in values:
                raise ValueError("Malformed or duplicate profile field")
            values[key] = value
        validate(values)
        return values
    except (OSError, UnicodeError, ValueError) as error:
        raise RuntimeError("Cannot read toolchain profile " + str(path) + ": " + str(error)
                           + ". Rerun setup to replace it after successful checks.") from error


def require_file(path):
    if not Path(path).is_file():
        raise RuntimeError("Remembered tool is unavailable: " + path + ". Rerun setup or explicitly select a tool.")
    return path


def selected_tool(key, values, environment):
    override = environment.get("VINC_BUILD_" + key.upper())
    if override:
        return override
    if values.get(key):
        return require_file(values[key])
    return key


def query(command):
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=30)
    if result.returncode:
        raise RuntimeError("Tool query failed: " + str(command[0]) + "\n" + result.stdout + result.stderr)
    return result.stdout.strip()


def save(values, root=ROOT):
    values = dict(values, format="1", platform=platform_key(), arch=architecture())
    validate(values)
    content = "".join(key + "=" + values[key] + "\n" for key in KEYS if key in values)
    if len(content.encode("utf-8")) > MAX_PROFILE_BYTES:
        raise ValueError("Profile exceeds size limit")
    # Write a single data file atomically. Launchers parse literal fields, never
    # source/eval it; no environment dump or executable shell fragments are saved.
    path = profile_path(root)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".profile-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return path


def apply(args, environment, values):
    """Apply only defaults; explicit CMake flags and environment choices win."""
    native_environment = os.name == "nt" and any(environment.get(name) for name in
        ("VSCMD_VER", "VCINSTALLDIR", "VCTOOLSINSTALLDIR"))
    if not native_environment:
        for variable, key in (("CC", "cc"), ("CXX", "cxx")):
            cmake_key = "CMAKE_C_COMPILER" if variable == "CC" else "CMAKE_CXX_COMPILER"
            if values.get(key) and not environment.get(variable) and cmake_key not in args.cache_values:
                environment[variable] = require_file(values[key])
    if values.get("developer_dir") and not environment.get("DEVELOPER_DIR"):
        directory = values["developer_dir"]
        if not Path(directory).is_dir():
            raise RuntimeError("Remembered Apple developer directory is unavailable: " + directory)
        environment["DEVELOPER_DIR"] = directory
    # Legacy profiles may contain module_python. Kconfig now owns that selection;
    # do not validate or inject the remembered path, even if it no longer exists.
    for cmake_key, key in (("Python3_EXECUTABLE", "config_python"),):
        if cmake_key not in args.cache_values and values.get(key):
            path = require_file(values[key])
            args.cache_values[cmake_key] = path
            args.definitions.append(cmake_key + "=" + path)
    # Node/npm must be selected together. These paths affect child build commands
    # only; a caller's persistent PATH is never edited.
    directories = []
    for key in ("node", "npm"):
        override = environment.get("VINC_BUILD_" + key.upper())
        if override or values.get(key):
            path = shutil.which(override, path=environment.get("PATH")) if override else require_file(values[key])
            if not path:
                raise RuntimeError("Explicit tool is unavailable: " + str(override))
            directory = str(Path(path).parent)
            if directory not in directories:
                directories.append(directory)
    environment["PATH"] = os.pathsep.join(directories + [environment.get("PATH", "")])


def main():
    parser = argparse.ArgumentParser(description="Save checked setup selections; not a build or installer.")
    for key in PATH_KEYS + ("toolset", "windows_sdk", "compiler_version"):
        parser.add_argument("--" + key.replace("_", "-"), default="")
    args = parser.parse_args()
    values = vars(args)
    for key in PATH_KEYS:
        if values[key] and key != "developer_dir":
            values[key] = os.path.abspath(require_file(values[key]))
    values["python_version"] = platform.python_version()
    for key in ("cmake", "ninja", "node"):
        if values[key]:
            values[key + "_version"] = query([values[key], "--version"]).splitlines()[0]
    print("Saved project toolchain: " + str(save(values)))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("Toolchain profile error: " + str(error), file=sys.stderr)
        sys.exit(1)

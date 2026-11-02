"""Shared build-tool Python policy; kept beside its data in source and SDK."""

from pathlib import Path
import re
import sys


def minimum_version():
    policy = Path(__file__).with_name("python_minimum.txt")
    value = policy.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"3\.[0-9]+", value):
        raise ValueError("Invalid Open-VINC Python version policy: " + str(policy))

    return tuple(map(int, value.split(".")))


def version_error(version=None, executable=None):
    version = sys.version_info if version is None else version
    executable = sys.executable if executable is None else executable
    minimum = minimum_version()
    if version[0] == 3 and tuple(version[:2]) >= minimum:
        return None

    required = ".".join(map(str, minimum))
    found = ".".join(map(str, version[:3]))
    return ("Python version is unsupported.\nRequired: Python >= " + required
            + " and < 4\nFound: " + found + "\nPath: " + str(executable)
            + "\nUpgrade Python or select a compatible interpreter explicitly."
            + "\nPrepare recommended dependencies: "
            + ("tools\\setup.bat" if sys.platform == "win32" else "bash tools/setup.sh")
            + " --recommended (in the Open-VINC source checkout)."
            + "\nBuild: VINC_BUILD_INTERPRETER; CMake tools: Python3_EXECUTABLE;"
            + " Python Modules: App Kconfig System/Custom selection; setup: VINC_SETUP_PYTHON.")


if __name__ == "__main__":
    try:
        error = version_error()
    except (OSError, ValueError) as failure:
        error = str(failure)

    if error:
        print(error, file=sys.stderr)
        sys.exit(1)

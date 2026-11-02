"""Check the selected interpreter, including files needed by Development.Module."""

import pathlib
import platform
import runpy
import struct
import sys
import sysconfig


def main():
    errors = []
    policy = pathlib.Path(__file__).resolve().parents[2] / "src/vinc/common/cmake/python_version.py"
    version_error = runpy.run_path(str(policy))["version_error"]()
    if version_error:
        errors.append(version_error)
    if platform.python_implementation() != "CPython":
        errors.append("CPython is required; found " + platform.python_implementation())
    if struct.calcsize("P") != 8:
        errors.append("64-bit Python is required")
    if sysconfig.get_config_var("Py_GIL_DISABLED"):
        errors.append("free-threaded Python is unsupported")
    if sys.platform == "win32" and sysconfig.get_platform() != "win-amd64":
        errors.append("x64 MSVC requires win-amd64 CPython, not MinGW/ARM Python")

    development = "--interpreter-only" not in sys.argv[1:]
    include = sysconfig.get_path("include")
    if development and (not include or not (pathlib.Path(include) / "Python.h").is_file()):
        errors.append("Python.h is missing; install development files for this interpreter")
    if development and sys.platform == "win32":
        library = pathlib.Path(sys.base_prefix) / "libs" / (
            "python{}{}.lib".format(sys.version_info.major, sys.version_info.minor)
        )
        if not library.is_file():
            errors.append("Python import library is missing: {}".format(library))

    if errors:
        print("Selected interpreter: " + sys.executable, file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("CPython {}: {}".format(platform.python_version(), sys.executable))
    return 0


if __name__ == "__main__":
    sys.exit(main())

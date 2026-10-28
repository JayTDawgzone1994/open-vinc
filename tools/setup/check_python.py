"""Check the selected interpreter, including files needed by Development.Module."""

import pathlib
import platform
import struct
import sys
import sysconfig


def main():
    errors = []
    if platform.python_implementation() != "CPython" or sys.version_info < (3, 9):
        errors.append("CPython 3.9 or newer is required")
    if struct.calcsize("P") != 8:
        errors.append("64-bit Python is required")
    if sysconfig.get_config_var("Py_GIL_DISABLED"):
        errors.append("free-threaded Python is unsupported")
    if sys.platform == "win32" and sysconfig.get_platform() != "win-amd64":
        errors.append("x64 MSVC requires win-amd64 CPython, not MinGW/ARM Python")

    include = sysconfig.get_path("include")
    if not include or not (pathlib.Path(include) / "Python.h").is_file():
        errors.append("Python.h is missing; install development files for this interpreter")
    if sys.platform == "win32":
        library = pathlib.Path(sys.base_prefix) / "libs" / (
            "python{}{}.lib".format(sys.version_info.major, sys.version_info.minor)
        )
        if not library.is_file():
            errors.append("Python import library is missing: {}".format(library))

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("CPython {}: {}".format(platform.python_version(), sys.executable))
    return 0


if __name__ == "__main__":
    sys.exit(main())

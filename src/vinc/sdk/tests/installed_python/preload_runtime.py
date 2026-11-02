# Prepended only to the fixture's deployed bootstrap, never production source.
import ctypes
import os
import sys

_old_runtime_fixture = None
if sys.platform == "win32" and os.environ.get("VINC_TEST_OLD_MSVC_RUNTIME"):
    _old_runtime_fixture = ctypes.WinDLL(os.environ["VINC_TEST_OLD_MSVC_RUNTIME"])

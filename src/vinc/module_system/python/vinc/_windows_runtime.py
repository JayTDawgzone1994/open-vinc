"""Private MSVC preflight. Runs before importing any VINC native code."""
import ctypes
import hashlib
import importlib.machinery
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import sys


# Keep successful loads alive for the process lifetime, including failed imports.
_runtime_handles = []
_MAX_PATH_CHARS = 32768
_INITIAL_MODULE_COUNT = 128
_MAX_ENUMERATION_ATTEMPTS = 8
_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x00000100
_LOAD_LIBRARY_SEARCH_SYSTEM32 = 0x00000800


class _FixedFileInfo(ctypes.Structure):
    # VS_FIXEDFILEINFO is thirteen DWORDs, independent of process pointer width.
    _fields_ = [(name, wintypes.DWORD) for name in (
        "signature", "structure_version", "file_ms", "file_ls", "product_ms",
        "product_ls", "flags_mask", "flags", "os", "type", "subtype",
        "date_ms", "date_ls")]


class _Windows:
    def __init__(self):
        self.kernel = ctypes.WinDLL("kernel32.dll", use_last_error=True,
                                    winmode=_LOAD_LIBRARY_SEARCH_SYSTEM32)
        self.version = ctypes.WinDLL("version.dll", use_last_error=True,
                                     winmode=_LOAD_LIBRARY_SEARCH_SYSTEM32)
        self._bind(self.kernel.GetCurrentProcess, [], wintypes.HANDLE)
        self._bind(self.kernel.K32EnumProcessModules,
                   [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
                    ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL)
        self._bind(self.kernel.GetModuleFileNameW,
                   [wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD], wintypes.DWORD)
        self._bind(self.kernel.GetSystemDirectoryW,
                   [wintypes.LPWSTR, wintypes.UINT], wintypes.UINT)
        self._bind(self.version.GetFileVersionInfoSizeW,
                   [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)], wintypes.DWORD)
        self._bind(self.version.GetFileVersionInfoW,
                   [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID], wintypes.BOOL)
        self._bind(self.version.VerQueryValueW,
                   [wintypes.LPCVOID, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p),
                    ctypes.POINTER(wintypes.UINT)], wintypes.BOOL)
        self._bind(self.kernel.SearchPathW,
                   [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                    wintypes.LPWSTR, ctypes.POINTER(wintypes.LPWSTR)], wintypes.DWORD)

    @staticmethod
    def _bind(function, arguments, result):
        function.argtypes = arguments
        function.restype = result

    def loaded(self):
        count = _INITIAL_MODULE_COUNT
        for _ in range(_MAX_ENUMERATION_ATTEMPTS):
            modules = (wintypes.HMODULE * count)()
            needed = wintypes.DWORD()
            if not self.kernel.K32EnumProcessModules(self.kernel.GetCurrentProcess(), modules,
                                                     ctypes.sizeof(modules), ctypes.byref(needed)):
                raise ctypes.WinError(ctypes.get_last_error())
            if needed.value > ctypes.sizeof(modules):
                count = needed.value // ctypes.sizeof(wintypes.HMODULE) + _INITIAL_MODULE_COUNT
                continue

            paths = []
            for index in range(needed.value // ctypes.sizeof(wintypes.HMODULE)):
                buffer = ctypes.create_unicode_buffer(_MAX_PATH_CHARS)
                length = self.kernel.GetModuleFileNameW(modules[index], buffer, len(buffer))
                if length == 0 or length >= len(buffer):
                    raise OSError("Cannot identify a loaded DLL; module list may have changed")
                paths.append(Path(buffer.value))
            return paths
        raise OSError("Loaded DLL list did not stabilize")

    def file_version(self, path):
        ignored = wintypes.DWORD()
        size = self.version.GetFileVersionInfoSizeW(str(path), ctypes.byref(ignored))
        if size == 0:
            raise OSError(f"Cannot read DLL version: {path}")
        buffer = ctypes.create_string_buffer(size)
        if not self.version.GetFileVersionInfoW(str(path), 0, size, buffer):
            raise ctypes.WinError(ctypes.get_last_error())
        address = ctypes.c_void_p()
        length = wintypes.UINT()
        if not self.version.VerQueryValueW(buffer, "\\", ctypes.byref(address), ctypes.byref(length)):
            raise OSError(f"Missing fixed DLL version: {path}")
        if length.value < ctypes.sizeof(_FixedFileInfo):
            raise OSError(f"Truncated DLL version: {path}")
        # VerQueryValueW returns a VS_FIXEDFILEINFO borrowed from buffer.
        info = ctypes.cast(address, ctypes.POINTER(_FixedFileInfo)).contents
        if info.signature != 0xFEEF04BD:
            raise OSError(f"Invalid DLL version signature: {path}")
        return (info.file_ms >> 16, info.file_ms & 0xFFFF,
                info.file_ls >> 16, info.file_ls & 0xFFFF)

    def candidate(self, name, debug):
        buffer = ctypes.create_unicode_buffer(_MAX_PATH_CHARS)
        length = self.kernel.GetSystemDirectoryW(buffer, len(buffer))
        if length == 0 or length >= len(buffer):
            raise OSError("Cannot identify Windows system directory")
        system_path = Path(buffer.value) / name
        if system_path.is_file() or not debug:
            return system_path

        # Debug CRTs are development-only. Search an explicit list, never CWD.
        # PATH is admitted only here, preserving an activated VS developer setup.
        directories = [str(Path(sys.executable).parent)]
        directories.extend(part for part in os.environ.get("PATH", "").split(os.pathsep)
                           if part and Path(part).is_absolute())
        length = self.kernel.SearchPathW(os.pathsep.join(directories), name, None,
                                         len(buffer), buffer, None)
        if length == 0 or length >= len(buffer):
            raise OSError(f"Debug Runtime {name} is missing; use a matching VS developer environment")
        return Path(buffer.value)

    @staticmethod
    def load(path):
        # Load dependencies beside the verified DLL or from safe default locations.
        # No process-wide search path change and no unloading of existing DLLs.
        handle = ctypes.WinDLL(str(path), use_last_error=True,
                               winmode=(_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                        _LOAD_LIBRARY_SEARCH_SYSTEM32))
        _runtime_handles.append(handle)


def _version_text(version):
    return ".".join(str(part) for part in version)


def _check(path, minimum, windows, *, loaded):
    actual = windows.file_version(path)
    if actual[0] != minimum[0] or actual < minimum:
        if loaded:
            remedy = (
                "This DLL is already loaded. Update the Python/package deployment that supplies it "
                "and restart Python; updating System32 alone cannot replace a loaded copy.")
        else:
            remedy = "Install a compatible Microsoft VC++ Runtime (or Debug development CRT) and restart Python."

        raise ImportError(
            f"VINC cannot load: incompatible MSVC Runtime '{path}': "
            f"{_version_text(actual)}; requires v{minimum[0]} >= {_version_text(minimum)}. "
            f"{remedy}"
        )


def _prepare(metadata, windows):
    versions = {name: tuple(version) for name, version in metadata["required"].items()}
    required = set(versions)

    def inspect_loaded():
        found = set()
        for path in windows.loaded():
            name = path.name.lower()
            if name in required:
                # Check every copy, not only the first DLL with this basename.
                _check(path, versions[name], windows, loaded=True)
                found.add(name)
        return found

    present = inspect_loaded()
    candidates = {}
    for name in sorted(required - present):
        path = windows.candidate(name, metadata["debug"])
        _check(path, versions[name], windows, loaded=False)
        candidates[name] = path

    # Validate all candidates before loading any. VCRUNTIME precedes the STL;
    # each load can also bring in dependencies, so recheck the complete list.
    order = sorted(candidates, key=lambda name: (not name.startswith("vcruntime"), name))
    for name in order:
        present = inspect_loaded()
        if name not in present:
            windows.load(candidates[name])
    missing = required - inspect_loaded()
    if missing:
        raise ImportError(f"VINC Runtime preparation incomplete: {', '.join(sorted(missing))}")


def _validate_metadata(metadata, mode="dynamic"):
    if mode == "static":
        _validate_static_metadata(metadata)
        return
    if mode != "dynamic":
        raise ValueError("Unknown MSVC Runtime linkage mode")
    if (type(metadata) is not dict or type(metadata.get("format")) is not int
            or metadata["format"] != 2 or type(metadata.get("required")) is not dict
            or not metadata["required"] or type(metadata.get("debug")) is not bool):
        raise ValueError("Invalid MSVC Runtime build metadata; rebuild/redeploy the complete Python binding")

    for name, version in metadata["required"].items():
        if (not isinstance(name, str)
                or re.fullmatch(r"(?:msvcp140|vcruntime140|concrt140)[a-z0-9_]*\.dll", name) is None):
            raise ValueError("Invalid MSVC Runtime DLL name")

        if (type(version) is not list or len(version) != 4
                or not all(type(part) is int and 0 <= part <= 65535 for part in version)
                or version[0] != 14):
            raise ValueError(f"Invalid MSVC Runtime DLL version: {name}")

    expected = "msvcp140d.dll" if metadata["debug"] else "msvcp140.dll"
    if expected not in metadata["required"]:
        raise ValueError(f"Missing required CRT classification: {expected}")


def _validate_static_metadata(metadata):
    if (type(metadata) is not dict or type(metadata.get("format")) is not int
            or metadata["format"] != 3 or metadata.get("mode") != "static"
            or type(metadata.get("required")) is not dict or metadata["required"]
            or type(metadata.get("debug")) is not bool):
        raise ValueError("Invalid static MSVC Runtime metadata; rebuild/redeploy the complete binding")
    native = metadata.get("native")
    if type(native) is not dict:
        raise ValueError("Missing static native image identity")
    filename = native.get("filename")
    digest = native.get("sha256")
    if (type(filename) is not str or re.fullmatch(r"_native[.A-Za-z0-9_-]*\.pyd", filename) is None
            or type(digest) is not str or re.fullmatch(r"[0-9a-f]{64}", digest) is None):
        raise ValueError("Invalid static native image identity")


def _verify_static_image(metadata, directory):
    # Detect a stale/mixed deployment before importing native code. This is not
    # a signature or a defense against someone rewriting the complete package.
    native = metadata["native"]
    expected = (directory / native["filename"]).resolve()
    selected = importlib.machinery.PathFinder.find_spec("_native", [str(directory)])
    if (selected is None or not isinstance(selected.loader, importlib.machinery.ExtensionFileLoader)
            or Path(selected.origin).resolve() != expected):
        raise ValueError("Python native image selection does not match Runtime metadata")
    with expected.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != native["sha256"]:
        raise ValueError("Native image does not match static Runtime metadata; redeploy the complete binding")


def prepare(mode="dynamic"):
    try:
        path = Path(__file__).with_name("_msvc_runtime.json")
        metadata = json.loads(path.read_text(encoding="utf-8"))
        _validate_metadata(metadata, mode)
        if mode == "static":
            _verify_static_image(metadata, path.parent)
        else:
            _prepare(metadata, _Windows())
    except ImportError:
        raise
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ImportError(f"VINC MSVC Runtime preflight failed: {error}") from error

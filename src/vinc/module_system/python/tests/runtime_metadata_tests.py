"""Deployment selection tests; no installed toolchain or DLL loading required."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


source = Path(__file__).resolve().parents[1] / "cmake/runtime_metadata.py"
spec = importlib.util.spec_from_file_location("runtime_metadata", source)
metadata = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metadata)
X64_MACHINE = 0x8664


def write_pe(path, machine=X64_MACHINE):
    header = bytearray(64)
    header[:2] = b"MZ"
    struct.pack_into("<I", header, 60, len(header))
    path.write_bytes(header + b"PE\0\0" + struct.pack("<H", machine))


class DeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.vc = Path(self.temporary.name) / "VC"
        self.toolset = self.vc / "Tools/MSVC/14.44.35207"
        self.compiler = self.toolset / "bin/HostX64/x64/cl.exe"
        self.compiler.parent.mkdir(parents=True)
        include = self.toolset / "include"
        include.mkdir()
        (include / "crtversion.h").write_text(
            "#define _VC_CRT_MAJOR_VERSION 14\n#define _VC_CRT_MINOR_VERSION 44\n"
            "#define _VC_CRT_BUILD_VERSION 35220\n#define _VC_CRT_RBUILD_VERSION 0\n")

    def redist(self, version):
        root = self.vc / "Redist/MSVC" / version
        root.mkdir(parents=True)
        return root

    def test_patch_numbers_do_not_replace_real_dll_versions(self):
        root = self.redist("14.44.35112")
        self.assertEqual(metadata.locate_redist(self.compiler), root)
        directory = root / "x64/Microsoft.VC143.CRT"
        directory.mkdir(parents=True)
        write_pe(directory / "msvcp140.dll")
        write_pe(directory / "vcruntime140.dll")
        versions = {"msvcp140.dll": (14, 44, 35211, 0), "vcruntime140.dll": (14, 44, 35210, 0)}
        actual = metadata.deployment_versions(
            directory, list(versions), X64_MACHINE, (14, 44), lambda path: versions[path.name])
        self.assertEqual(actual, {name: list(version) for name, version in versions.items()})

    def test_latest_compatible_package_in_same_installation(self):
        self.redist("14.42.99999")
        self.redist("14.44.35112")
        self.redist("15.0.99999")
        self.redist("v145")
        latest = self.redist("14.51.36231")
        self.assertEqual(metadata.locate_redist(self.compiler), latest)

    def test_no_fallback_to_old_package_or_header(self):
        with self.assertRaisesRegex(RuntimeError, "No compatible"):
            metadata.locate_redist(self.compiler)
        self.redist("14.42.99999")
        with self.assertRaisesRegex(RuntimeError, "No compatible"):
            metadata.locate_redist(self.compiler)

    def test_debug_release_and_architecture_are_separate(self):
        root = self.redist("14.44.35112")
        release = root / "x64/Microsoft.VC143.CRT"
        debug = root / "debug_nonredist/x64/Microsoft.VC143.DebugCRT"
        release.mkdir(parents=True)
        debug.mkdir(parents=True)
        self.assertEqual(metadata.deployment_directory(root, "x64", False), release)
        self.assertEqual(metadata.deployment_directory(root, "x64", True), debug)
        with self.assertRaises(RuntimeError):
            metadata.deployment_directory(root, "arm64", False)

    def test_missing_corrupt_wrong_architecture_and_old_files_fail(self):
        directory = self.redist("14.44.35112")
        names = ["msvcp140.dll"]
        read_version = lambda path: (14, 44, 35211, 0)
        with self.assertRaises(FileNotFoundError):
            metadata.deployment_versions(directory, names, X64_MACHINE, (14, 44), read_version)
        path = directory / names[0]
        path.write_bytes(b"corrupt")
        with self.assertRaisesRegex(RuntimeError, "Invalid PE"):
            metadata.deployment_versions(directory, names, X64_MACHINE, (14, 44), read_version)
        write_pe(path, 0x014C)
        with self.assertRaisesRegex(RuntimeError, "architecture mismatch"):
            metadata.deployment_versions(directory, names, X64_MACHINE, (14, 44), read_version)
        write_pe(path)
        with self.assertRaisesRegex(RuntimeError, "predates"):
            metadata.deployment_versions(directory, names, X64_MACHINE, (14, 44),
                                         lambda path: (14, 42, 34438, 0))

    def test_static_has_no_redist_requirement_and_identifies_image(self):
        native = Path(self.temporary.name) / "_native.cp313-win_amd64.pyd"
        write_pe(native)
        with patch.object(metadata, "imported_libraries", return_value=["kernel32.dll", "python313.dll"]):
            result = metadata.static_metadata(self.compiler, native, "Release")
        self.assertEqual(result["format"], 3)
        self.assertEqual(result["mode"], "static")
        self.assertEqual(result["required"], {})
        self.assertFalse(result["debug"])
        self.assertEqual(result["native"]["filename"], native.name)
        self.assertEqual(len(result["native"]["sha256"]), 64)
        self.assertFalse((self.vc / "Redist").exists())

    def test_static_rejects_crt_sidecars_ucrt_and_unreviewed_dependencies(self):
        baseline = ["kernel32.dll", "ws2_32.dll", "python313.dll"]
        metadata.validate_static_imports(baseline)
        for name in ("msvcp140.dll", "msvcp140d.dll", "msvcp140_atomic_wait.dll",
                     "vcruntime140_1.dll", "concrt140.dll", "ucrtbase.dll",
                     "api-ms-win-crt-runtime-l1-1-0.dll", "foreign.dll"):
            with self.subTest(name=name):
                with self.assertRaisesRegex(RuntimeError, "unreviewed"):
                    metadata.validate_static_imports(baseline + [name])
        for names in ([], ["kernel32.dll"], baseline + ["python311.dll"]):
            with self.assertRaises(RuntimeError):
                metadata.validate_static_imports(names)


if __name__ == "__main__":
    unittest.main(verbosity=2)

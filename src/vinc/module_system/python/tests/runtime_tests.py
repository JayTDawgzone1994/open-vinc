"""Loader policy tests and isolated-process checks; never alter installed DLLs."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
from copy import deepcopy
from unittest.mock import patch


PACKAGE = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location("runtime_preflight", PACKAGE / "vinc/_windows_runtime.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
CHILD_TIMEOUT_SECONDS = 30
MINIMUM = (14, 44, 35000, 0)
OLD = (14, 29, 30157, 0)
NEW = (14, 51, 36247, 0)


class FakeWindows:
    def __init__(self, paths=()):
        self.paths = list(paths)
        self.versions = {path: NEW for path in paths}
        self.loads = []
        self.missing = set()

    def loaded(self):
        return list(self.paths)

    def file_version(self, path):
        if path.name in self.missing:
            raise OSError(f"Missing version: {path}")
        return self.versions.get(path, NEW)

    def candidate(self, name, debug):
        return Path("system") / name

    def load(self, path):
        self.loads.append(path)
        self.paths.append(path)


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.metadata = {"format": 2,
                         "required": {"msvcp140.dll": list(MINIMUM), "vcruntime140.dll": list(MINIMUM)},
                         "debug": False}

    def test_preload_only_missing_and_check_actual_result(self):
        existing = Path("python/vcruntime140.dll")
        windows = FakeWindows([existing])
        runtime._prepare(self.metadata, windows)
        self.assertEqual(windows.loads, [Path("system/msvcp140.dll")])

    def test_each_dll_uses_its_own_deployment_floor(self):
        windows = FakeWindows()
        stl = Path("system/msvcp140.dll")
        crt = Path("system/vcruntime140.dll")
        self.metadata["required"]["msvcp140.dll"] = [14, 44, 35211, 0]
        self.metadata["required"]["vcruntime140.dll"] = [14, 44, 35210, 0]
        windows.versions[stl] = (14, 44, 35211, 0)
        windows.versions[crt] = (14, 44, 35210, 0)
        runtime._prepare(self.metadata, windows)
        self.assertEqual(set(windows.loads), {stl, crt})

        windows.versions[crt] = (14, 44, 35209, 0)
        with self.assertRaisesRegex(ImportError, "already loaded"):
            runtime._prepare(self.metadata, windows)

    def test_python_old_runtime_remains_rejected(self):
        path = Path("python/vcruntime140.dll")
        windows = FakeWindows([path])
        windows.versions[path] = (14, 42, 34438, 0)
        with self.assertRaisesRegex(ImportError, "System32 alone cannot replace"):
            runtime._prepare(self.metadata, windows)
        self.assertEqual(windows.loads, [])

    def test_manifest_requires_complete_per_dll_versions(self):
        runtime._validate_metadata(self.metadata)
        variants = []
        for version in (None, [], [14, 44, 1], [14, 44, -1, 0], [14, True, 1, 0],
                        [14, 44, 65536, 0], [15, 0, 1, 0], "14.44.35211.0"):
            metadata = deepcopy(self.metadata)
            metadata["required"]["msvcp140.dll"] = version
            variants.append(metadata)
        for format_value in (1, True, 99):
            metadata = deepcopy(self.metadata)
            metadata["format"] = format_value
            variants.append(metadata)
        for names in ({}, [], {"../msvcp140.dll": list(MINIMUM)},
                      {"msvcp140d.dll": list(MINIMUM)}):
            metadata = deepcopy(self.metadata)
            metadata["required"] = names
            variants.append(metadata)
        for metadata in variants:
            with self.subTest(metadata=metadata):
                with self.assertRaises(ValueError):
                    runtime._validate_metadata(metadata)

    def test_reject_every_old_copy_before_loading_anything(self):
        first = Path("system/msvcp140.dll")
        second = Path("winrt/msvcp140.dll")
        windows = FakeWindows([first, second])
        windows.versions[second] = OLD
        with self.assertRaisesRegex(ImportError, "14.29.30157.0"):
            runtime._prepare(self.metadata, windows)
        self.assertEqual(windows.loads, [])

    def test_missing_version_does_not_partially_preload(self):
        windows = FakeWindows()
        windows.missing.add("vcruntime140.dll")
        with self.assertRaises(OSError):
            runtime._prepare(self.metadata, windows)
        self.assertEqual(windows.loads, [])

    def test_old_system_candidate_is_not_loaded(self):
        windows = FakeWindows()
        windows.versions[Path("system/msvcp140.dll")] = OLD
        with self.assertRaisesRegex(ImportError, "requires v14"):
            runtime._prepare(self.metadata, windows)
        self.assertEqual(windows.loads, [])

    def test_failed_load_propagates(self):
        windows = FakeWindows()

        def fail_load(path):
            raise OSError(f"Load failed: {path}")

        windows.load = fail_load
        with self.assertRaisesRegex(OSError, "Load failed"):
            runtime._prepare(self.metadata, windows)

    def test_recheck_dependencies_loaded_as_side_effect(self):
        windows = FakeWindows()
        original_load = windows.load

        def load_with_old_dependency(path):
            original_load(path)
            dependency = Path("thirdparty/msvcp140.dll")
            windows.paths.append(dependency)
            windows.versions[dependency] = OLD

        windows.load = load_with_old_dependency
        with self.assertRaises(ImportError):
            runtime._prepare(self.metadata, windows)

    def test_refuse_other_major_version(self):
        path = Path("system/msvcp140.dll")
        windows = FakeWindows([path])
        windows.versions[path] = (15, 0, 0, 0)
        with self.assertRaises(ImportError):
            runtime._prepare(self.metadata, windows)

    def test_debug_uses_its_own_dependencies(self):
        windows = FakeWindows([Path("python/msvcp140.dll")])
        windows.versions[windows.paths[0]] = OLD
        self.metadata["required"] = {"msvcp140d.dll": list(MINIMUM)}
        self.metadata["debug"] = True
        runtime._prepare(self.metadata, windows)
        self.assertEqual(windows.loads, [Path("system/msvcp140d.dll")])


class StaticPolicyTests(unittest.TestCase):
    def setUp(self):
        self.metadata = {"format": 3, "mode": "static", "required": {}, "debug": False,
                         "native": {"filename": "_native.cp313-win_amd64.pyd", "sha256": "a" * 64}}

    def test_static_cannot_be_selected_by_dynamic_build(self):
        runtime._validate_metadata(self.metadata, "static")
        with self.assertRaises(ValueError):
            runtime._validate_metadata(self.metadata, "dynamic")
        with self.assertRaises(ValueError):
            runtime._validate_metadata(self.metadata, "unknown")
        with self.assertRaises(ValueError):
            runtime._validate_metadata({"format": 2, "required": {}, "debug": False}, "static")

    def test_corrupt_static_metadata_is_rejected(self):
        changes = [("format", True), ("format", 99), ("mode", "dynamic"), ("mode", None),
                   ("required", []), ("required", {"msvcp140.dll": [14, 44, 0, 0]}),
                   ("debug", 0), ("native", None),
                   ("native", {"filename": "../_native.pyd", "sha256": "a" * 64}),
                   ("native", {"filename": "_native.pyd", "sha256": "x" * 64})]
        for key, value in changes:
            with self.subTest(key=key, value=value):
                metadata = deepcopy(self.metadata)
                metadata[key] = value
                with self.assertRaises(ValueError):
                    runtime._validate_metadata(metadata, "static")

    def test_static_preflight_never_inspects_or_loads_crt(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "_msvc_runtime.json"
            manifest.write_text(json.dumps(self.metadata), encoding="utf-8")
            with patch.object(runtime, "__file__", str(Path(directory) / "_windows_runtime.py")), \
                    patch.object(runtime, "_Windows", side_effect=AssertionError("CRT touched")), \
                    patch.object(runtime, "_verify_static_image") as verify:
                runtime.prepare("static")
                verify.assert_called_once()

    def test_hash_and_selected_extension_must_match(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            native = directory / self.metadata["native"]["filename"]
            native.write_bytes(b"test image")
            self.metadata["native"]["sha256"] = hashlib.sha256(native.read_bytes()).hexdigest()
            spec = importlib.util.spec_from_file_location("_native", native,
                loader=runtime.importlib.machinery.ExtensionFileLoader("_native", str(native)))
            with patch.object(runtime.importlib.machinery.PathFinder, "find_spec", return_value=spec):
                runtime._verify_static_image(self.metadata, directory)
                native.write_bytes(b"different image")
                with self.assertRaisesRegex(ValueError, "does not match"):
                    runtime._verify_static_image(self.metadata, directory)
            with patch.object(runtime.importlib.machinery.PathFinder, "find_spec", return_value=None):
                with self.assertRaisesRegex(ValueError, "selection"):
                    runtime._verify_static_image(self.metadata, directory)


@unittest.skipUnless(sys.platform == "win32", "Windows MSVC integration")
class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.metadata = json.loads((PACKAGE / "vinc/_msvc_runtime.json").read_text())
        cls.static = cls.metadata.get("mode") == "static"

    def run_child(self, code, *arguments):
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(PACKAGE)
        result = subprocess.run([sys.executable, "-X", "faulthandler", "-c", textwrap.dedent(code), *arguments],
                                env=environment, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=CHILD_TIMEOUT_SECONDS)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_normal_import_and_native_messages(self):
        self.run_child("""
            import runpy
            import sys
            test = sys.argv[1]
            sys.argv = [test, sys.argv[2]]
            runpy.run_path(test, run_name='__main__')
            """,
                       str(Path(__file__).with_name("message_tests.py")), str(PACKAGE))

    def test_real_windows_enumeration_and_versions(self):
        self.run_child("""
            import vinc
            from vinc._windows_runtime import _Windows
            windows = _Windows()
            paths = [path for path in windows.loaded() if path.name.lower().startswith('msvcp140')]
            from vinc import _build
            if _build.MSVC_RUNTIME_MODE == 'dynamic':
                assert paths
            print([(str(path), windows.file_version(path)) for path in paths])
            """)

    def test_corrupt_metadata_refuses_before_native(self):
        with tempfile.TemporaryDirectory(prefix="vinc-runtime-") as directory:
            package = Path(directory) / "vinc"
            package.mkdir()
            for name in ("__init__.py", "_build.py", "_windows_runtime.py", "bootstrap.py"):
                (package / name).write_bytes((PACKAGE / "vinc" / name).read_bytes())
            (package / "_msvc_runtime.json").write_text('{"format": 99}', encoding="utf-8")
            self.run_child("""
                import sys
                sys.path.insert(0, sys.argv[1])
                try:
                    import vinc
                except ImportError as error:
                    assert 'preflight' in str(error)
                    assert 'vinc._native' not in sys.modules
                else:
                    raise AssertionError('invalid metadata accepted')
                """, directory)
            # The central-launched bootstrap must emit the diagnostic and exit,
            # even though native supervision has not been initialized yet.
            result = subprocess.run([sys.executable, str(package / "bootstrap.py")],
                                    capture_output=True, timeout=CHILD_TIMEOUT_SECONDS)
            self.assertEqual(result.returncode, 70)
            self.assertIn(b"preflight", result.stderr)

    def test_static_stale_image_and_mode_refuse_before_native(self):
        if not self.static:
            self.skipTest("Static deployment validation")
        with tempfile.TemporaryDirectory(prefix="vinc-runtime-stale-") as directory:
            package = Path(directory) / "vinc"
            package.mkdir()
            for name in ("__init__.py", "_build.py", "_windows_runtime.py", "bootstrap.py"):
                (package / name).write_bytes((PACKAGE / "vinc" / name).read_bytes())
            native = package / self.metadata["native"]["filename"]
            native.write_bytes(b"stale native artifact")
            manifest = package / "_msvc_runtime.json"
            for metadata in (self.metadata, {"format": 2, "required": {"msvcp140.dll": list(MINIMUM)}, "debug": False}):
                manifest.write_text(json.dumps(metadata), encoding="utf-8")
                self.run_child("""
                    import sys
                    sys.path.insert(0, sys.argv[1])
                    try:
                        import vinc
                    except ImportError as error:
                        assert 'preflight' in str(error), str(error)
                        assert 'vinc._native' not in sys.modules
                    else:
                        raise AssertionError('mixed deployment accepted')
                    """, directory)
            manifest.unlink()
            self.run_child("""
                import sys
                sys.path.insert(0, sys.argv[1])
                try:
                    import vinc
                except ImportError as error:
                    assert 'preflight' in str(error), str(error)
                    assert 'vinc._native' not in sys.modules
                else:
                    raise AssertionError('missing metadata accepted')
                """, directory)

    def test_real_old_dll_obeys_linkage_policy(self):
        old_dll = os.environ.get("VINC_TEST_OLD_MSVC_RUNTIME")
        metadata = json.loads((PACKAGE / "vinc/_msvc_runtime.json").read_text())
        if not old_dll or (metadata["debug"] and not self.static):
            self.skipTest("Requires an explicit old Release CRT test fixture")
        self.run_child("""
            import ctypes
            import sys
            dll = ctypes.WinDLL(sys.argv[1])
            if sys.argv[2] == 'static':
                import vinc
                from vinc._windows_runtime import _Windows
                assert any(path.resolve() == __import__('pathlib').Path(sys.argv[1]).resolve()
                           for path in _Windows().loaded())
                raise SystemExit(0)
            try:
                import vinc
            except ImportError as error:
                assert 'incompatible MSVC Runtime' in str(error), str(error)
                assert 'vinc._native' not in sys.modules
                print(error)
            else:
                raise AssertionError('old DLL accepted')
            """, old_dll, 'static' if self.static else 'dynamic')

    def test_winrt_import_order(self):
        winrt = os.environ.get("VINC_TEST_WINRT_DIR")
        metadata = json.loads((PACKAGE / "vinc/_msvc_runtime.json").read_text())
        if not winrt or (metadata["debug"] and not self.static):
            self.skipTest("Requires an isolated WinRT wheel directory for Release")
        if self.static:
            for first in ("winrt", "vinc"):
                self.run_child("""
                    import sys
                    import runpy
                    sys.path.insert(0, sys.argv[1])
                    if sys.argv[2] == 'winrt':
                        import winrt.system
                    import vinc
                    import winrt.system
                    assert list(winrt.system.Array('i', [1, 2, 3])) == [1, 2, 3]
                    from vinc._windows_runtime import _Windows
                    paths = [path for path in _Windows().loaded() if path.name.lower() == 'msvcp140.dll']
                    assert any('winrt' in str(path).lower() for path in paths), paths
                    test = sys.argv[3]
                    sys.argv = [test, sys.argv[4]]
                    runpy.run_path(test, run_name='__main__')
                    """, winrt, first, str(Path(__file__).with_name("message_tests.py")), str(PACKAGE))
            return
        self.run_child("""
            import sys
            sys.path.insert(0, sys.argv[1])
            import winrt.system
            try:
                import vinc
            except ImportError as error:
                assert 'incompatible MSVC Runtime' in str(error)
                assert 'vinc._native' not in sys.modules
            else:
                raise AssertionError('WinRT-first should be rejected')
            """, winrt)
        self.run_child("""
            import sys
            import runpy
            sys.path.insert(0, sys.argv[1])
            import vinc
            import winrt.system
            assert list(winrt.system.Array('i', [1, 2, 3])) == [1, 2, 3]
            from vinc._windows_runtime import _Windows
            assert not any('winrt' in str(path).lower() for path in _Windows().loaded()
                           if path.name.lower() == 'msvcp140.dll')
            test = sys.argv[2]
            sys.argv = [test, sys.argv[3]]
            runpy.run_path(test, run_name='__main__')
            """, winrt, str(Path(__file__).with_name("message_tests.py")), str(PACKAGE))
        self.run_child("""
            import sys
            import runpy
            sys.path.insert(0, sys.argv[1])
            helper = runpy.run_path(sys.argv[2])
            helper['prepare']()
            import winrt.system
            import vinc
            assert list(winrt.system.Array('i', [1, 2, 3])) == [1, 2, 3]
            test = sys.argv[3]
            sys.argv = [test, sys.argv[4]]
            runpy.run_path(test, run_name='__main__')
            """, winrt, str(PACKAGE / "vinc/_windows_runtime.py"),
                       str(Path(__file__).with_name("message_tests.py")), str(PACKAGE))


if __name__ == "__main__":
    unittest.main(verbosity=2)

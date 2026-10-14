"""Loader policy tests and isolated-process checks; never alter installed DLLs."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest


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
        self.metadata = {"minimum": MINIMUM, "required": ["msvcp140.dll", "vcruntime140.dll"],
                         "debug": False}

    def test_preload_only_missing_and_check_actual_result(self):
        existing = Path("python/vcruntime140.dll")
        windows = FakeWindows([existing])
        runtime._prepare(self.metadata, windows)
        self.assertEqual(windows.loads, [Path("system/msvcp140.dll")])

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
        self.metadata["required"] = ["msvcp140d.dll"]
        self.metadata["debug"] = True
        runtime._prepare(self.metadata, windows)
        self.assertEqual(windows.loads, [Path("system/msvcp140d.dll")])


@unittest.skipUnless(sys.platform == "win32", "Windows MSVC integration")
class NativeTests(unittest.TestCase):
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

    def test_real_old_dll_rejected_before_native(self):
        old_dll = os.environ.get("VINC_TEST_OLD_MSVC_RUNTIME")
        metadata = json.loads((PACKAGE / "vinc/_msvc_runtime.json").read_text())
        if not old_dll or metadata["debug"]:
            self.skipTest("Requires an explicit old Release CRT test fixture")
        self.run_child("""
            import ctypes
            import sys
            dll = ctypes.WinDLL(sys.argv[1])
            try:
                import vinc
            except ImportError as error:
                assert 'incompatible MSVC Runtime' in str(error), str(error)
                assert 'vinc._native' not in sys.modules
                print(error)
            else:
                raise AssertionError('old DLL accepted')
            """, old_dll)

    def test_winrt_import_order(self):
        winrt = os.environ.get("VINC_TEST_WINRT_DIR")
        metadata = json.loads((PACKAGE / "vinc/_msvc_runtime.json").read_text())
        if not winrt or metadata["debug"]:
            self.skipTest("Requires an isolated WinRT wheel directory for Release")
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

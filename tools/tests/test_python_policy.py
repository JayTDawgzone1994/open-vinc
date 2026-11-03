"""Python minimum acceptance and early refusal; no installers are executed."""

import importlib.util
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
POLICY_DIR = ROOT / "src/vinc/common/cmake"
POLICY = runpy.run_path(str(POLICY_DIR / "python_version.py"))


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)


class PythonPolicyTests(unittest.TestCase):
    def test_version_boundaries_and_diagnostics(self):
        for version in ((3, 9, 99), (3, 10, 99), (4, 0, 0)):
            error = POLICY["version_error"](version, "/chosen/python")
            self.assertIn("Required: Python >= 3.11 and < 4", error)
            self.assertIn("Found: " + ".".join(map(str, version)), error)
            self.assertIn("Path: /chosen/python", error)
            self.assertIn("--recommended", error)
        for version in ((3, 11, 0), (3, 11, 18), (3, 13, 12)):
            self.assertIsNone(POLICY["version_error"](version))

    def test_old_interpreter_fails_before_build_side_effects(self):
        # Only version discovery is simulated. Execute the real build entry in a
        # child and ensure it never reaches helpers, configure or build creation.
        code = (
            "import runpy,sys; sys.version_info=(3,10,99); "
            "sys.argv=[sys.argv[1]]; runpy.run_path(sys.argv[0], run_name='__main__')"
        )
        with tempfile.TemporaryDirectory() as directory:
            result = run([sys.executable, "-B", "-c", code, str(ROOT / "build.py")], directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Found: 3.10.99", result.stderr)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_setup_interpreter_only_does_not_require_headers(self):
        spec = importlib.util.spec_from_file_location("setup_probe", ROOT / "tools/setup/check_python.py")
        probe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        with mock.patch.object(probe.sysconfig, "get_path", return_value="/missing/headers"):
            with mock.patch.object(sys, "argv", ["check_python.py", "--interpreter-only"]):
                self.assertEqual(probe.main(), 0)
            with mock.patch.object(sys, "argv", ["check_python.py", "--with-development"]):
                self.assertEqual(probe.main(), 1)

    def test_import_refuses_before_native_or_runtime_loader(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / "vinc"
            package.mkdir()
            shutil.copy2(ROOT / "src/vinc/module_system/python/vinc/__init__.py", package)
            (package / "_build.py").write_text("MINIMUM_VERSION = (3, 99)\nMSVC_RUNTIME = True\n")
            result = run([sys.executable, "-B", "-c", "import vinc"], directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("VINC requires Python >= 3.99", result.stderr)
            self.assertNotIn("No module named", result.stderr)

    @unittest.skipUnless(shutil.which("cmake"), "CMake required")
    def test_cmake_source_and_relocated_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            policy = root / "moved SDK/lib/cmake/vinc"
            policy.mkdir(parents=True)
            for name in ("VincPythonRequirements.cmake", "python_minimum.txt", "python_version.py"):
                shutil.copy2(POLICY_DIR / name, policy)
            for index, helper in enumerate((POLICY_DIR, policy)):
                source = root / str(index)
                source.mkdir()
                (source / "CMakeLists.txt").write_text(
                    'cmake_minimum_required(VERSION 3.20)\nproject(Check NONE)\n'
                    f'include("{helper.as_posix()}/VincPythonRequirements.cmake")\n'
                    f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")\n'
                    'vinc_find_python(Python3 REQUIRED COMPONENTS Interpreter)\n', encoding="utf-8")
                result = run(["cmake", "-S", str(source), "-B", str(source / "build")], root)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which("cmake"), "CMake required")
    def test_cmake_rejection_keeps_optional_probe_optional(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # A FindPython test double exercises diagnostics/REQUIRED semantics,
            # not a claim of running an actual Python 3.10 interpreter.
            (root / "FindPython3.cmake").write_text(
                'set(Python3_FOUND FALSE)\nset(Python3_VERSION 3.10.99)\n'
                'set(Python3_EXECUTABLE /selected/old-python)\n'
                'set(Python3_NOT_FOUND_MESSAGE "Unsuitable version")\n')
            for mode in ("QUIET", "REQUIRED"):
                (root / "CMakeLists.txt").write_text(
                    'cmake_minimum_required(VERSION 3.20)\nproject(Check NONE)\n'
                    f'include("{POLICY_DIR.as_posix()}/VincPythonRequirements.cmake")\n'
                    'list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")\n'
                    f'vinc_find_python(Python3 {mode} COMPONENTS Interpreter)\n')
                result = run(["cmake", "-S", str(root), "-B", str(root / mode)], root)
                output = result.stdout + result.stderr
                self.assertEqual(result.returncode == 0, mode == "QUIET", output)
                if mode == "REQUIRED":
                    self.assertIn("Found: 3.10.99", output)
                    self.assertIn("Path: /selected/old-python", output)
                    self.assertIn("Python3_EXECUTABLE", output)

    @unittest.skipUnless(shutil.which("cmake"), "CMake required")
    def test_cmake_missing_explicit_interpreter_suggests_setup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing = root / "missing-python"
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(Check NONE)\n'
                f'include("{POLICY_DIR.as_posix()}/VincPythonRequirements.cmake")\n'
                f'set(Python3_EXECUTABLE "{missing.as_posix()}")\n'
                'vinc_find_python(Python3 REQUIRED COMPONENTS Interpreter)\n')
            result = run(["cmake", "-S", str(root), "-B", str(root / "build")], root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(missing.as_posix(), result.stderr)
            self.assertIn("bash tools/setup.sh", result.stderr)
            self.assertNotIn("--recommended", result.stderr)


if __name__ == "__main__":
    unittest.main()

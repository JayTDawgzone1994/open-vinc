"""Exercise the real CMake selection and cache contracts without compiling VINC."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HELPER = ROOT / "src/vinc/common/cmake/VincPythonSelection.cmake"


def literal(value):
    delimiter = "="
    while "]" + delimiter + "]" in value:
        delimiter += "="
    return "[" + delimiter + "[" + str(value) + "]" + delimiter + "]"


@unittest.skipUnless(shutil.which("cmake"), "CMake required")
class PythonSelectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vinc-python-selection-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cmake = shutil.which("cmake")
        self.ninja = shutil.which("ninja")
        if not self.ninja:
            self.skipTest("Ninja required for compiler-independent fixtures")

    def configure(self, body, *, success=True, flags=(), env=None):
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(Selection NONE)\n'
            f'include({literal(HELPER.as_posix())})\nvinc_reject_python_override()\n' + body,
            encoding="utf-8")
        result = subprocess.run([self.cmake, "-S", str(self.root), "-B", str(self.root / "build"),
                                 "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + self.ninja, *flags],
                                env=env, capture_output=True, text=True, encoding="utf-8", timeout=30)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode == 0, success, output)
        return output

    def custom(self, path=None, app="First"):
        return ('set(VINC_PYTHON_CUSTOM ON)\n'
                f'set(VINC_PYTHON_EXECUTABLE {literal(path or Path(sys.executable).as_posix())})\n'
                f'vinc_select_module_python({app})\n')

    def test_custom_selection_and_repeat_configuration(self):
        self.assertIn("App First: Module Python", self.configure(self.custom()))
        self.configure(self.custom())
        cache = (self.root / "build/CMakeCache.txt").read_text(encoding="utf-8")
        self.assertNotIn("\nPython_EXECUTABLE:", cache)

    def test_system_and_custom_share_first_path_interpreter(self):
        env = dict(os.environ, PATH=str(Path(sys.executable).parent) + os.pathsep + os.environ.get("PATH", ""))
        body = 'set(VINC_PYTHON_CUSTOM OFF)\nvinc_select_module_python(SystemApp)\n'
        body += 'set(first "${VINC_SELECTED_PYTHON}")\n'
        body += 'set(VINC_PYTHON_CUSTOM ON)\nset(VINC_PYTHON_EXECUTABLE "${first}")\n'
        body += 'vinc_select_module_python(CustomApp)\n'
        output = self.configure(body, env=env)
        self.assertIn("App CustomApp: Module Python", output)

    def test_missing_system_python_does_not_search_registry_or_other_paths(self):
        native_tools = str(Path(os.environ["SystemRoot"]) / "System32") if os.name == "nt" else ""
        output = self.configure('set(VINC_PYTHON_CUSTOM OFF)\nvinc_select_module_python(Missing)\n',
                                env=dict(os.environ, PATH=native_tools), success=False)
        self.assertIn("Module Python not found", output)

    def test_missing_custom_python_is_not_replaced(self):
        output = self.configure(self.custom((self.root / "missing-python").as_posix()), success=False)
        self.assertIn("Module Python not found", output)

    def test_public_cmake_input_rejected_even_when_equal(self):
        output = self.configure(self.custom(), flags=["-DPython_EXECUTABLE=" + sys.executable], success=False)
        self.assertIn("no longer an Open-VINC setting", output)
        self.configure(self.custom(), flags=["-U", "Python_EXECUTABLE"])

    def test_changed_cached_abi_is_rejected(self):
        self.configure(self.custom())
        output = self.configure(self.custom(), flags=["-D_VINC_MODULE_PYTHON_ABI:INTERNAL=3.0/8/old"], success=False)
        self.assertIn("environment differs from this build cache", output)

    def test_minimum_version_failure_explains_selection(self):
        # Use the actual interpreter with an intentionally newer test policy;
        # this tests diagnostics, not a claim of running an obsolete CPython.
        for name in ("VincPythonSelection.cmake", "VincPythonRequirements.cmake"):
            shutil.copy2(HELPER.parent / name, self.root / name)
        (self.root / "python_minimum.txt").write_text("3.99\n")
        body = f'include({literal((self.root / "VincPythonSelection.cmake").as_posix())})\n'
        output = self.configure(body + self.custom(), success=False)
        self.assertIn("requires 64-bit GIL-enabled CPython >= 3.99", output)

    def test_missing_development_files_uses_kconfig_diagnostic(self):
        # FindPython failure injection exercises the shared REQUIRED diagnostic.
        (self.root / "FindPython.cmake").write_text(
            'set(Python_FOUND FALSE)\nset(Python_NOT_FOUND_MESSAGE "missing Development.Module")\n')
        body = self.custom() + 'set(Python_EXECUTABLE "${VINC_SELECTED_PYTHON}")\n'
        body += 'list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")\n'
        body += 'vinc_find_python(Python REQUIRED COMPONENTS Interpreter Development.Module)\n'
        output = self.configure(body, success=False)
        self.assertIn("missing Development.Module", output)
        self.assertIn("Select System/Custom Module Python in App Kconfig", output)

    def test_two_real_environments_conflict_and_preserve_venv_identity(self):
        venv = self.root / "中文 environment"
        subprocess.run([sys.executable, "-m", "venv", "--without-pip", str(venv)], check=True, timeout=30)
        interpreter = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python3")
        output = self.configure(self.custom() + self.custom(interpreter.as_posix(), "Second"), success=False)
        self.assertIn("Apps in one build must use the same Python interpreter", output)
        self.assertIn("Second", output)
        output = self.configure(self.custom(interpreter.as_posix()), success=False)
        self.assertIn("environment differs from this build cache", output)


if __name__ == "__main__":
    unittest.main()

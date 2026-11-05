"""Validate the public Python Module recipe with real CMake/Kconfig helpers."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[4]
CONFIGURE_TIMEOUT_SECONDS = 45


class PythonModuleRegistration(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vinc python 空白-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.build = self.root / "build"
        (self.source / "detector.py").write_text("raise RuntimeError('configure executed user code')\n", encoding="utf-8")
        (self.source / "resources").mkdir()
        (self.source / "resources/規則.json").write_text("{}", encoding="utf-8")
        self.declaration = ('add_python_module(detector_python MODULE_TYPE_NAME detector '
                            'SOURCES detector.py resources/規則.json)\n')
        self.prefix = f'''cmake_minimum_required(VERSION 3.20)
project(python_registration LANGUAGES NONE)
include("{REPOSITORY.as_posix()}/src/vinc/module_system/cmake/VincModule.cmake")
'''

    def configure(self, declaration=None, *, diagnostic=None, suffix=""):
        body = self.declaration if declaration is None else declaration
        (self.source / "CMakeLists.txt").write_text(self.prefix + body + suffix, encoding="utf-8")
        result = subprocess.run([shutil.which("cmake"), "-S", str(self.source), "-B", str(self.build),
                                 f"-DPython3_EXECUTABLE={sys.executable}"],
                                capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=CONFIGURE_TIMEOUT_SECONDS)
        output = result.stdout + result.stderr
        if diagnostic is None:
            self.assertEqual(result.returncode, 0, output)
        else:
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn(diagnostic, output)

    def test_relative_layout_and_catalog_exclusion(self):
        self.configure(suffix='''
get_target_property(paths detector_python VINC_PYTHON_MODULE_PATHS)
if(NOT paths STREQUAL "detector.py;resources/規則.json")
    message(FATAL_ERROR "Relative resource layout changed")
endif()
vinc_render_module_catalog(body DEPLOYED detector_python)
if(NOT body STREQUAL "")
    message(FATAL_ERROR "Python target entered native catalog")
endif()
''')

    def test_required_entry(self):
        self.configure(self.declaration.replace("SOURCES detector.py", "SOURCES"), diagnostic="requires entry detector.py")

    def test_missing_file(self):
        self.configure(self.declaration.replace("detector.py resources", "missing.py resources"), diagnostic="Missing Python Module source file")

    def test_unsupported_keyword(self):
        self.configure(self.declaration.replace("SOURCES", "SOURCE_FILE"), diagnostic="requires MODULE_TYPE_NAME and SOURCES")

    def test_parent_traversal(self):
        (self.root / "outside.py").write_text("", encoding="utf-8")
        self.configure(self.declaration.replace("resources/規則.json", "../outside.py"), diagnostic="must be relative files")

    def test_absolute_source(self):
        absolute = (self.source / "detector.py").as_posix()
        self.configure(self.declaration.replace("SOURCES detector.py", f'SOURCES "{absolute}"'), diagnostic="must be relative files")

    def test_duplicate_destination(self):
        self.configure(self.declaration.replace("resources/規則.json", "./detector.py"), diagnostic="paths collide")

    def test_symlink_escape(self):
        outside = self.root / "outside.py"
        outside.write_text("", encoding="utf-8")
        try:
            (self.source / "linked.py").symlink_to(outside)
        except OSError as error:
            self.skipTest(f"Symlinks unavailable in this environment: {error}")
        self.configure(self.declaration.replace("resources/規則.json", "linked.py"), diagnostic="escapes its directory")

    def test_case_collision(self):
        self.configure(suffix='add_python_module(other MODULE_TYPE_NAME Detector SOURCES detector.py)',
                       diagnostic="case-insensitive deployment collision")

    def test_cross_language_collision(self):
        self.configure(suffix='add_module(other MODULE_TYPE_NAME detector SOURCES anything.cpp)',
                       diagnostic="Duplicate ModuleType")

    def test_reserved_type(self):
        self.configure(self.declaration.replace("MODULE_TYPE_NAME detector", "MODULE_TYPE_NAME WebUI"),
                       diagnostic="reserved for the official")

    def app_configuration(self, enabled):
        (self.source / "First.config").write_text("# CONFIG_VINC_APP_PYTHON is not set\n", encoding="utf-8")
        (self.source / "Second.config").write_text(
            f"CONFIG_VINC_APP_PYTHON={'y' if enabled else 'n'}\nCONFIG_VINC_MODULE_detector=y\n", encoding="utf-8")
        return f'''
include("{REPOSITORY.as_posix()}/src/vinc/sdk/cmake/VincConfiguration.cmake")
foreach(app First Second)
    add_custom_target(${{app}})
    vinc_register_app_configuration(${{app}} CONFIG "${{app}}.config"
        TOOL_DIRECTORY "{REPOSITORY.as_posix()}/tools/configure"
        UI_AVAILABLE TRUE BAG_AVAILABLE TRUE PYTHON_AVAILABLE TRUE)
endforeach()
function(check_selection)
    get_target_property(first First VINC_APP_MODULE_TARGETS)
    get_target_property(second Second VINC_APP_MODULE_TARGETS)
    if(first OR NOT second STREQUAL "detector_python")
        message(FATAL_ERROR "Python Module selection escaped its App")
    endif()
endfunction()
cmake_language(DEFER CALL check_selection)
'''

    def test_per_app_selection(self):
        self.configure(suffix=self.app_configuration(True))

    def test_selection_requires_python(self):
        self.configure(suffix=self.app_configuration(False), diagnostic="requires CONFIG_VINC_APP_PYTHON=y")


if __name__ == "__main__":
    unittest.main()

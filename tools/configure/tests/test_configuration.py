"""Contract tests for app settings; upstream remains unchanged."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "configure.py"
spec = importlib.util.spec_from_file_location("vinc_configurator", TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.config = self.root / "vinc.conf"

    def run_tool(self, *arguments, success=True):
        result = subprocess.run(
            [sys.executable, "-B", str(TOOL), "--config", str(self.config), *arguments],
            capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_defaults_do_not_write_user_file(self):
        output = self.run_tool("--output", str(self.root / "build"))
        self.assertIn("CONFIG_VINC_APP_BAG=y", output)
        self.assertIn("CONFIG_VINC_APP_UI=y", output)
        self.assertIn("CONFIG_VINC_APP_PYTHON=n", output)
        self.assertFalse(self.config.exists())

    def test_save_round_trip_and_stable_generated_file(self):
        self.run_tool("--save", "--set", "VINC_APP_UI=n")
        build = self.root / "build"
        self.run_tool("--output", str(build))
        generated = build / "features.cmake"
        timestamp = generated.stat().st_mtime_ns
        self.run_tool("--output", str(build))
        self.assertEqual(timestamp, generated.stat().st_mtime_ns)
        self.assertIn("set(VINC_APP_UI OFF)", generated.read_text())
        self.assertIn("set(VINC_APP_BAG ON)", generated.read_text())

    def test_invalid_inputs_are_not_normalized_away(self):
        cases = [
            ("CONFIG_BAD=y\n", "unknown option"),
            ("CONFIG_VINC_APP_UI=yes\n", "requires y or n"),
            ("CONFIG_VINC_APP_UI=n\nCONFIG_VINC_APP_UI=y\n", "duplicate"),
            ("set(VINC_APP_UI ON)\n", "invalid configuration syntax"),
        ]
        for text, diagnostic in cases:
            with self.subTest(text=text):
                self.config.write_text(text)
                self.assertIn(diagnostic, self.run_tool(success=False))
                self.assertEqual(text, self.config.read_text())

    def test_missing_explicit_file(self):
        self.assertIn("does not exist", self.run_tool("--require-file", success=False))

    def test_capabilities_and_legacy_conflicts(self):
        self.config.write_text("CONFIG_VINC_APP_UI=y\n")
        self.assertIn("unavailable", self.run_tool("--ui-available", "n", success=False))
        self.assertIn("conflicts", self.run_tool("--legacy", "VINC_APP_UI=n", success=False))
        self.config.write_text("CONFIG_VINC_APP_PYTHON=y\n")
        self.assertIn("unavailable", self.run_tool("--python-available", "n", success=False))

    def test_gui_public_save_uses_same_validation(self):
        config = module.ApplicationConfig({"UI": False, "BAG": True, "PYTHON": False})
        with self.assertRaises(OSError):
            config.write_config(str(self.config))
        self.assertFalse(self.config.exists())
        config.syms["VINC_APP_UI"].set_value("n")
        config.write_config(str(self.config))
        self.assertIn("CONFIG_VINC_APP_UI is not set", self.config.read_text())
        self.run_tool("--ui-available", "n", "--python-available", "n")

    def test_legacy_inputs_cannot_silently_override_each_other(self):
        self.assertIn("conflicting legacy", self.run_tool(
            "--legacy", "VINC_APP_BAG=y", "--legacy", "VINC_APP_BAG=n", success=False))

    def test_editor_cannot_save_against_legacy_input(self):
        config = module.ApplicationConfig({"UI": True, "BAG": True, "PYTHON": True})
        config.legacy_values = {"VINC_APP_UI": "n"}
        with self.assertRaisesRegex(OSError, "legacy CMake"):
            config.write_config(str(self.config))
        self.assertFalse(self.config.exists())

    def write_modules(self, names):
        manifest = self.root / "modules.json"
        manifest.write_text(json.dumps(names), encoding="utf-8")
        return str(manifest)

    def test_modules_default_off_and_official_features_are_not_switches(self):
        manifest = self.write_modules(["Camera", "Detector"])
        output = self.run_tool("--modules", manifest)
        self.assertIn("CONFIG_VINC_MODULE_Camera=n", output)
        self.assertIn("CONFIG_VINC_MODULE_Detector=n", output)
        self.assertNotIn("CONFIG_VINC_APP_UI", output)
        self.assertNotIn("CONFIG_VINC_APP_BAG", output)
        self.assertFalse(self.config.exists())
        self.config.write_text("CONFIG_VINC_APP_UI=n\n")
        self.assertIn("unknown option", self.run_tool("--modules", manifest, success=False))

    def test_module_identity_survives_reordering_and_is_case_sensitive(self):
        manifest = self.write_modules(["Camera", "camera", "Other"])
        self.run_tool("--modules", manifest, "--save", "--set", "VINC_MODULE_Camera=y")
        original = self.config.read_bytes()
        self.write_modules(["Other", "camera", "Camera"])
        output = self.run_tool("--modules", manifest)
        self.assertIn("CONFIG_VINC_MODULE_Camera=y", output)
        self.assertIn("CONFIG_VINC_MODULE_camera=n", output)
        self.assertEqual(self.config.read_bytes(), original)

    def test_unknown_removed_and_invalid_modules_are_rejected(self):
        self.config.write_text("CONFIG_VINC_MODULE_Removed=y\n")
        manifest = self.write_modules(["Camera"])
        self.assertIn("unknown option", self.run_tool("--modules", manifest, success=False))
        self.config.unlink()
        for names in (["Camera", "Camera"], ["bad-name"], [3], None, {}):
            with self.subTest(names=names):
                manifest = self.write_modules(names)
                self.run_tool("--modules", manifest, success=False)

    def test_per_app_save_and_generated_outputs_are_isolated(self):
        manifest = self.write_modules(["Camera", "Detector"])
        self.run_tool("--modules", manifest, "--save", "--set", "VINC_MODULE_Camera=y")
        first = self.config.read_bytes()
        self.run_tool("--modules", manifest, "--output", str(self.root / "first"))
        self.config = self.root / "second.conf"
        self.run_tool("--modules", manifest, "--save", "--set", "VINC_MODULE_Detector=y")
        self.run_tool("--modules", manifest, "--output", str(self.root / "second"))
        self.assertEqual((self.root / "vinc.conf").read_bytes(), first)
        first_output = (self.root / "first/features.cmake").read_text()
        second_output = (self.root / "second/features.cmake").read_text()
        self.assertIn("set(VINC_MODULE_Camera ON)", first_output)
        self.assertIn("set(VINC_MODULE_Camera OFF)", second_output)

    def test_official_capabilities_are_required_in_composition(self):
        manifest = self.write_modules([])
        for feature in ("ui", "bag"):
            self.assertIn("required but unavailable", self.run_tool(
                "--modules", manifest, f"--{feature}-available", "n", success=False))
        self.config.write_text("CONFIG_VINC_APP_PYTHON=y\n")
        self.assertIn("unavailable", self.run_tool(
            "--modules", manifest, "--python-available", "n", success=False))

    def test_editor_save_and_open_validate_dynamic_modules(self):
        config = module.ApplicationConfig({"UI": True, "BAG": True, "PYTHON": False}, ["Camera"])
        self.addCleanup(config.schema_directory.cleanup)
        config.syms["VINC_MODULE_Camera"].set_value("y")
        config.write_config(str(self.config))
        config.load_config(str(self.config))
        self.assertEqual(config.syms["VINC_MODULE_Camera"].str_value, "y")
        config.syms["VINC_APP_PYTHON"].set_value("y")
        saved = self.config.read_bytes()
        with self.assertRaisesRegex(OSError, "unavailable"):
            config.write_config(str(self.config))
        self.assertEqual(saved, self.config.read_bytes())
        self.config.write_text("CONFIG_VINC_MODULE_Removed=y\n")
        with self.assertRaisesRegex(OSError, "unknown option"):
            config.load_config(str(self.config))

    def test_preflight_resolves_features_but_final_pass_rejects_unknown_module(self):
        self.config.write_text("CONFIG_VINC_APP_PYTHON=y\nCONFIG_VINC_MODULE_Late=y\n")
        original = self.config.read_bytes()
        self.assertIn("CONFIG_VINC_APP_PYTHON=y", self.run_tool("--preflight"))
        self.assertEqual(self.config.read_bytes(), original)
        self.assertIn("unknown option", self.run_tool(
            "--modules", self.write_modules([]), success=False))
        self.assertIn("unavailable", self.run_tool(
            "--preflight", "--python-available", "n", success=False))
        self.run_tool("--preflight", "--save", success=False)


if __name__ == "__main__":
    unittest.main()

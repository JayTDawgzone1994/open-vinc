"""Application feature configuration; CMake remains the build authority."""

import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile

sys.dont_write_bytecode = True
TOOL_DIR = Path(__file__).resolve().parent
VENDORED = TOOL_DIR / "kconfiglib"
if not VENDORED.is_dir():
    VENDORED = TOOL_DIR.parents[1] / "library/kconfiglib/upstream"
sys.path.insert(0, str(VENDORED))
try:
    import kconfiglib
except ImportError:
    sys.exit("Kconfiglib is missing; restore library/kconfiglib/upstream or reinstall the SDK")


def assignments(path, symbols):
    """Validate the user file before Kconfig can normalize or ignore entries."""
    requested = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        disabled = re.fullmatch(r"# CONFIG_(\w+) is not set", line)
        enabled = re.fullmatch(r"CONFIG_(\w+)=(.*)", line)
        if disabled:
            name, value = disabled[1], "n"
        elif enabled:
            name, value = enabled[1], enabled[2]
        elif line.startswith("#"):
            continue
        else:
            raise ValueError(f"{path}:{line_number}: invalid configuration syntax")
        if name not in symbols:
            raise ValueError(f"{path}:{line_number}: unknown option CONFIG_{name}")
        if name in requested:
            raise ValueError(f"{path}:{line_number}: duplicate CONFIG_{name}")
        if symbols[name].type != kconfiglib.BOOL or value not in ("y", "n"):
            raise ValueError(f"{path}:{line_number}: CONFIG_{name} requires y or n")
        requested[name] = value
    return requested


class ApplicationConfig(kconfiglib.Kconfig):
    """Keep upstream UI saves/loads subject to the same contract as CLI/CMake."""

    def __init__(self, capabilities, modules=None):
        self.schema_directory = None
        schema = TOOL_DIR / "Kconfig"
        self.module_composition = modules is not None
        if self.module_composition:
            # Keep the generated schema alive throughout an upstream editor session.
            # It contains only validated identifiers, never user-supplied Kconfig code.
            self.schema_directory = tempfile.TemporaryDirectory(prefix="vinc-kconfig-")
            schema = Path(self.schema_directory.name) / "Kconfig"
            schema.write_text(module_schema(modules), encoding="utf-8")
        super().__init__(str(schema), warn_to_stderr=False)
        self.capabilities = capabilities
        self.legacy_values = {}

    def load_config(self, filename=None, replace=True, verbose=None):
        path = Path(filename or kconfiglib.standard_config_filename())
        if path.exists():
            try:
                requested = assignments(path, self.syms)
            except ValueError as error:
                raise OSError(str(error)) from error
            result = super().load_config(str(path), replace=replace)
            for name, value in requested.items():
                if self.syms[name].str_value != value:
                    raise OSError(f"{path}: CONFIG_{name} requested {value}, effective {self.syms[name].str_value}")
            return result
        return super().load_config(str(path), replace=replace)

    def validate(self):
        for name, value in self.legacy_values.items():
            if self.syms[name].str_value != value:
                raise OSError(f"CONFIG_{name} conflicts with legacy CMake option ({value}); remove the legacy input")
        for feature, available in self.capabilities.items():
            name = f"VINC_APP_{feature}"
            if self.module_composition and feature in ("UI", "BAG"):
                if not available:
                    raise OSError(f"Official {feature} is required but unavailable in this SDK/build")
                continue
            if self.syms[name].str_value == "y" and not available:
                raise OSError(f"CONFIG_{name}=y is unavailable in this SDK/build; disable it or use a capable SDK")
        for symbol in self.unique_defined_syms:
            if symbol.user_value is not None:
                requested = kconfiglib.TRI_TO_STR[symbol.user_value]
                if requested != symbol.str_value:
                    raise OSError(f"CONFIG_{symbol.name}: requested {requested}, effective {symbol.str_value}")

    def write_config(self, filename=None, header=None, save_old=True, verbose=None):
        self.validate()
        return super().write_config(filename, header, save_old)

    def write_min_config(self, filename, header=None):
        self.validate()
        return super().write_min_config(filename, header)


def write_if_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        temporary = path.with_suffix(path.suffix + ".tmp")
        # Path.write_text(newline=...) requires Python 3.10; keep the 3.9 floor.
        with temporary.open("w", encoding="utf-8", newline="\n") as output:
            output.write(text)
        temporary.replace(path)


def module_schema(modules):
    """ModuleType is the stable configuration identity, not the CMake target name."""
    if not isinstance(modules, list):
        raise ValueError("Module manifest must be a JSON array of ModuleType names")
    seen = set()
    for name in modules:
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise ValueError(f"Invalid ModuleType in configuration manifest: {name!r}")
        if name in seen:
            raise ValueError(f"Duplicate ModuleType in configuration manifest: {name}")
        seen.add(name)
    lines = [
        'mainmenu "Open-VINC application composition"',
        (TOOL_DIR / "Kconfig.python").read_text(encoding="utf-8"),
        'menu "Modules"',
    ]
    for name in sorted(seen):
        lines.extend([
            f"config VINC_MODULE_{name}",
            f'    bool "{name}"',
            "    default n",
        ])
    lines.append("endmenu")
    return "\n\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    capabilities_path = TOOL_DIR / "capabilities.json"
    capabilities = {"UI": True, "BAG": True, "PYTHON": True}
    if capabilities_path.exists():
        capabilities = json.loads(capabilities_path.read_text(encoding="utf-8"))
        if not isinstance(capabilities, dict) or any(
                type(capabilities.get(name)) is not bool for name in ("UI", "BAG", "PYTHON")):
            raise ValueError(f"Invalid SDK capability metadata: {capabilities_path}")
    parser.add_argument("--config", type=Path, default=Path.cwd() / "vinc.conf")
    parser.add_argument("--require-file", action="store_true")
    parser.add_argument("--output", type=Path, help="Build-only output directory")
    parser.add_argument("--modules", type=Path,
                        help="Generated JSON ModuleType list; selects the per-App composition schema")
    parser.add_argument("--preflight", action="store_true",
                        help="Internal: resolve features before Module registration; not membership validation")
    for feature in ("UI", "PYTHON", "BAG"):
        parser.add_argument(f"--{feature.lower()}-available", choices=("y", "n"),
                            default="y" if capabilities[feature] else "n")
    parser.add_argument("--legacy", action="append", default=[], metavar="OPTION=y|n")
    parser.add_argument("--set", action="append", default=[], metavar="OPTION=y|n")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--gui", action="store_true")
    mode.add_argument("--menu", action="store_true")
    mode.add_argument("--save", action="store_true", help="Explicitly save effective choices")
    args = parser.parse_args()
    if args.preflight and (args.modules or args.gui or args.menu or args.save or args.set or args.legacy):
        parser.error("--preflight is a read-only feature pass without a Module manifest")
    args.config = args.config.resolve()
    if args.output and args.config == (args.output / ".config").resolve():
        parser.error("build output .config must not overwrite the user configuration")
    if args.require_file and not args.config.is_file():
        parser.error(f"configuration file does not exist: {args.config}")
    if args.set and not args.save:
        parser.error("--set requires --save")
    modules = None
    if args.preflight:
        # CMake must create dependency subdirectories before deferred composition.
        # Provisional symbols permit late registration; the final manifest pass
        # still rejects every unknown/removed Module before generation succeeds.
        modules = sorted(set(re.findall(
            r"(?:^|\n)\s*(?:# )?CONFIG_VINC_MODULE_([A-Za-z_][A-Za-z0-9_]*)(?:=| is not set)",
            args.config.read_text(encoding="utf-8-sig"))))
    elif args.modules is not None:
        modules = json.loads(args.modules.read_text(encoding="utf-8"))
        if not isinstance(modules, list):
            raise ValueError("Module manifest must be a JSON array of ModuleType names")
    config = ApplicationConfig({
        "UI": args.ui_available == "y",
        "PYTHON": args.python_available == "y",
        "BAG": args.bag_available == "y",
    }, modules)
    requested = {}
    if args.config.exists():
        requested = assignments(args.config, config.syms)
        config.load_config(str(args.config))
    legacy_values = {}
    for entry in args.legacy:
        name, value = parse_assignment(entry, config)
        if name in legacy_values and legacy_values[name] != value:
            raise ValueError(f"CONFIG_{name}: conflicting legacy CMake options")
        legacy_values[name] = value
        if name in requested and requested[name] != value:
            raise ValueError(f"{args.config}: CONFIG_{name} conflicts with legacy CMake option ({value})")
        config.syms[name].set_value(value)
    config.legacy_values = legacy_values
    for entry in args.set:
        name, value = parse_assignment(entry, config)
        config.syms[name].set_value(value)

    if args.gui or args.menu:
        # Upstream owns the editor. Saves and file opens call the validated public
        # Kconfig methods above; no UI patch or duplicated parser is necessary.
        os.environ["KCONFIG_CONFIG"] = str(args.config)
        if args.gui:
            import guiconfig
            guiconfig.menuconfig(config)
        else:
            import menuconfig
            menuconfig.menuconfig(config)
        return

    config.validate()
    if args.save:
        args.config.parent.mkdir(parents=True, exist_ok=True)
        config.write_config(str(args.config))
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        config.write_config(str(args.output / ".config"), save_old=False)
        cmake = "# Generated from Kconfig; do not edit.\n"
        for symbol in config.unique_defined_syms:
            value = "ON" if symbol.str_value == "y" else "OFF"
            cmake += f"set({symbol.name} {value})\n"
        write_if_changed(args.output / "features.cmake", cmake)
    print(f"Vinc application configuration: {args.config}")
    for symbol in config.unique_defined_syms:
        print(f"  CONFIG_{symbol.name}={symbol.str_value}")


def parse_assignment(entry, config):
    name, separator, value = entry.partition("=")
    if name.startswith("CONFIG_"):
        name = name[len("CONFIG_"):]
    if not separator or name not in config.syms or value not in ("y", "n"):
        raise ValueError(f"Invalid assignment: {entry}; expected known OPTION=y|n")
    return name, value


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, kconfiglib.KconfigError, ImportError) as error:
        sys.exit(f"Vinc configuration error: {error}")

"""Run the real examples through the central Shell in an isolated deployment."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib

RUN_TIMEOUT_SECONDS = 45
CALLBACK_SETTLE_MS = 200
COMPLETION_SETTLE_MS = 500


def run_script(executable, root, commands):
    script = root / "smoke.tcl"
    result_file = root / "result.txt"
    result_file.unlink(missing_ok=True)
    script.write_text(
        "set failure [catch {\n" + "\n".join(commands) +
        '\n} detail]\nset report [open "result.txt" w]\n'
        'puts $report "$failure $detail"\nclose $report\nexit\n', encoding="utf-8")
    result = subprocess.run(
        [str(executable), "--shell-port", "0", "--python", sys.executable, "-S", str(script)],
        cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=RUN_TIMEOUT_SECONDS)
    output = result.stdout.decode("utf-8", errors="replace")
    print(output)
    assert result.returncode == 0, output
    report = result_file.read_text()
    assert report.startswith("0 "), (report, output)
    return output


def check_initial(output, native, peer):
    for name, value in (("native", native), ("peer", peer)):
        match = re.search(rf"INITIAL {name} gain=([^ ]+) default=([^\s]+)", output)
        assert match and float(match[1]) == value and float(match[2]) == 1.0, output


def parameter_workflow(executable, root):
    files = list((root / "vinc_data").glob("*/params.toml"))
    assert len(files) == 1, files
    file = files[0]

    def saved():
        return tomllib.loads(file.read_text(encoding="utf-8"))["params"]

    # The first run's explicit args and subsequent live edits did not save.
    for name in ("native.gain", "peer.gain"):
        assert saved()[name]["default"] == saved()[name]["value"] == 1.0
    starts = ["userExampleModule --name native", "userExamplePython --name peer"]
    output = run_script(executable, root, starts + [
        "param set native.gain 4.0", "param set peer.gain 5.0", "param save"])
    check_initial(output, 1.0, 1.0)
    output = run_script(executable, root, [
        "userExampleModule --name native --gain 6.0",
        "userExamplePython --name peer --gain 7.0"])
    check_initial(output, 6.0, 7.0)
    assert saved()["native.gain"]["value"] == 4.0
    assert saved()["peer.gain"]["value"] == 5.0

    # Simulate offline editing, including an inactive Module. These are valid
    # typed doubles but invalid business values; the whole file must survive.
    file.write_text("""format = 1
[params.'native.gain']
type = 'double'
default = 1.0
value = -2.0
[params.'peer.gain']
type = 'double'
default = 1.0
value = -3.0
[params.'retired.gain']
type = 'double'
default = 1.0
value = 9.0
""", encoding="utf-8")
    output = run_script(executable, root, starts)
    check_initial(output, 1.0, 1.0)
    assert "CORRECT native" in output and "CORRECT peer" in output, output
    assert saved()["native.gain"]["value"] == -2.0
    assert saved()["peer.gain"]["value"] == -3.0
    assert saved()["retired.gain"]["value"] == 9.0
    # Restart corrects again; only the explicit save persists the replacements.
    output = run_script(executable, root, starts + ["param save"])
    check_initial(output, 1.0, 1.0)
    assert saved()["native.gain"]["value"] == 1.0
    assert saved()["peer.gain"]["value"] == 1.0
    assert saved()["retired.gain"]["value"] == 9.0
    print("Param startup/default/live/save/correction workflows passed")


def restart_hint_workflow(executable, root):
    modules = (("userExampleModule", "hintNative"), ("userExamplePython", "hintPython"))

    def start_and_status():
        commands = []
        for module_type, name in modules:
            commands.extend([f"{module_type} --name {name}", f"{name} status"])
        return commands

    def check_labels(output, expected):
        for _, name in modules:
            assert re.search(rf"STATUS {name} mid=\d+ .*?startup_label={expected}", output), output

    changes = []
    for _, name in modules:
        changes.extend([f"param get {name}.label", f"param set {name}.label revised", f"{name} status"])
    output = run_script(executable, root, start_and_status() + changes)
    for _, name in modules:
        assert f"[restart: Module '{name}']" in output, output
        assert f"Restart hint: Module '{name}'" in output, output
        states = re.findall(rf"STATUS {name} mid=(\d+) .*?startup_label=([^\s]+)", output)
        assert len(states) == 2 and states[0] == states[1] and states[0][1] == "example", output
    files = list((root / "vinc_data").glob("*/params.toml"))
    assert len(files) == 1, files
    saved = tomllib.loads(files[0].read_text(encoding="utf-8"))["params"]
    for _, name in modules:
        assert saved[f"{name}.label"]["value"] == "example", saved
        assert "restart_hint" not in saved[f"{name}.label"], saved

    # Tcl reports successful commands as 0 and prints their messages separately.
    # Do not parse a MID from its return value or guess assigned MID numbers.
    output = run_script(executable, root, start_and_status() + changes + ["param save"])
    check_labels(output, "example")
    output = run_script(executable, root, start_and_status())
    check_labels(output, "revised")
    print("C++ / Python restart hints, deferred application and explicit save workflows passed")


def correction_commands():
    # Each command waits for the prior response. A command following the Module's
    # set runs in a later pump batch, after its Param callback has corrected gain.
    return ["native set gain -2.0", "native status",
            "peer set gain -3.0", "peer status",
            "param get native.gain", "param get peer.gain"]


def main():
    original = Path(sys.argv[1]).resolve()
    examples = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="vinc-user-example-") as temporary:
        root = Path(temporary)
        executable = root / original.name
        shutil.copy2(original, executable)
        for library in original.parent.glob("*.dll"):
            shutil.copy2(library, root / library.name)
        shutil.copytree(original.parent / "lib", root / "lib")
        shutil.copytree(original.parent / "python/vinc", root / "python/vinc")
        shutil.copytree(original.parent / "modules", root / "modules")
        # Copy UI assets only, never the developer's params/logs/instance records.
        for assets in (original.parent / "vinc_data").glob("*/ui"):
            shutil.copytree(assets, root / "vinc_data" / assets.parent.name / "ui")
        shutil.copytree(examples / "python", root / "data/module")

        # Tcl catches deliberately failing commands. Unexpected failure still closes
        # the central, then becomes a test failure via the result file.
        commands = [
            'if {![catch {param set native.gain 2.0}]} {error "unregistered set succeeded"}',
            'if {![catch {userExampleModule --name badNative --gain -1}]} {error "invalid native args accepted"}',
            'if {![catch {userExamplePython --name badPython --gain -1}]} {error "invalid Python args accepted"}',
            "userExampleModule --name native --topic userExample/test --gain 2.0",
            "userExamplePython --name peer --topic userExample/test --gain 3.0",
            "native help", "peer help", "native storage", "peer storage",
            'if {![catch {native last}]} {error "empty native last succeeded"}',
            'if {![catch {peer last}]} {error "empty Python last succeeded"}',
            "native capacity", "peer capacity",
            "native set locked_gain 2.0", "peer set locked_gain 2.0",
            'if {![catch {native set peer.locked_gain 2.0}]} {error "native foreign permission bypass"}',
            'if {![catch {peer set native.locked_gain 2.0}]} {error "Python foreign permission bypass"}',
            "native push 101", f"after {CALLBACK_SETTLE_MS}", "peer last",
            "peer loan 202", f"after {CALLBACK_SETTLE_MS}", "native last",
            "native loan 303", f"after {CALLBACK_SETTLE_MS}", "peer last",
            "peer push 404", f"after {CALLBACK_SETTLE_MS}", "native last",
            "native topics", "peer topics",
            "native set peer.gain 2.5", "peer set native.gain 3.5",
            f"after {CALLBACK_SETTLE_MS}", "native params", "peer params",
            "param set native.locked_gain 4.0", "param set peer.locked_gain 4.0",
            "native logs", "peer logs", "native status", "peer status",
            "native reopen", "peer reopen", "native push 505",
            f"after {CALLBACK_SETTLE_MS}", "peer last",
            "userManualModule", "manualCpp status", "manualCpp finish",
            "userManualPython --name manualPy", "manualPy status", "manualPy finish",
            f"after {COMPLETION_SETTLE_MS}",
            "userManualModule loop", "manualCpp status",
            "userManualPython --name loopPy loop", "loopPy status",
        ]
        output = run_script(executable, root, commands + correction_commands())
        check_initial(output, 2.0, 3.0)
        assert "CORRECT native" in output and "CORRECT peer" in output, output
        for name in ("native", "peer"):
            assert f"{name}.gain [double] = 1 (default 1," in output, output
        for marker in ("READY native", "READY peer", "STORAGE_OK", "EXPECTED checked access",
                       "EXPECTED capacity", "NESTED id=7 label=camera", "CACHE gain=3.5",
                       "CACHE gain=2.5", "TOPIC userExample/test", "RAW printf", "RAW print",
                       "MANUAL manualCpp", "MANUAL manualPy", "MANUAL loopPy",
                       "STOPPED native", "STOPPED peer"):
            assert marker in output, (marker, output)
        for receiver, sequence, publisher in (("peer", 101, "native"), ("native", 202, "peer"),
                                               ("peer", 303, "native"), ("native", 404, "peer")):
            pattern = rf"CALLBACK {receiver} MESSAGE sequence={sequence} .*publisher={publisher} "
            assert re.search(pattern, output), (pattern, output)
        assert output.count("STORAGE_OK") == 2, output
        assert output.count("EXPECTED capacity") == 2, output
        parameter_workflow(executable, root)
        restart_hint_workflow(executable, root)
        print("C++ / Python user examples passed")


if __name__ == "__main__":
    main()

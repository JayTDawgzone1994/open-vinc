"""Run the real examples through the central Shell in an isolated deployment."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

RUN_TIMEOUT_SECONDS = 45
CALLBACK_SETTLE_MS = 200
COMPLETION_SETTLE_MS = 500


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
        shutil.copytree(examples / "python", root / "data/module")

        # Tcl catches deliberately failing commands. Unexpected failure still closes
        # the central, then becomes a test failure via the result file.
        commands = [
            "userExampleModule --name native --topic userExample/test",
            "userExamplePython --name peer --topic userExample/test",
            "native help", "peer help", "native storage", "peer storage",
            'if {![catch {native last}]} {error "empty native last succeeded"}',
            'if {![catch {peer last}]} {error "empty Python last succeeded"}',
            "native capacity", "peer capacity",
            'if {![catch {native set locked_gain 2.0}]} {error "native permission bypass"}',
            'if {![catch {peer set locked_gain 2.0}]} {error "Python permission bypass"}',
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
        script = root / "smoke.tcl"
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
        assert (root / "result.txt").read_text().startswith("0 "), output
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
        assert "Traceback (most recent call last)" not in output, output
        print("C++ / Python user examples passed")


if __name__ == "__main__":
    main()

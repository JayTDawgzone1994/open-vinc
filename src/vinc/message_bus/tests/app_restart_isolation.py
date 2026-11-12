"""Two live Apps use identical Module and Topic names in one isolated journal root."""

import os
from contextlib import ExitStack
from pathlib import Path
from queue import Empty, Queue
import subprocess
import sys
import tempfile
from threading import Thread
import time


TIMEOUT = 15
POLL_SECONDS = 0.025
NO_PUBLISHED_DATA = -102  # message_bus_module_helper command result


class App:
    def __init__(self, stack, executable, plugin, name, root, *options):
        env = dict(os.environ, HOME=str(root), TMP=str(root), TEMP=str(root))
        self.process = subprocess.Popen(
            [executable, name, plugin, *options], cwd=root, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
        )
        self.replies = Queue()
        self.reader = Thread(target=self.read_replies, daemon=True)
        self.reader.start()
        stack.callback(self.close)

    def read_replies(self):
        for line in self.process.stdout:
            self.replies.put(line.rstrip())
        self.replies.put(None)

    def reply(self):
        try:
            value = self.replies.get(timeout=TIMEOUT)
        except Empty as error:
            raise AssertionError(f"central {self.process.pid} did not reply") from error
        if value is None:
            raise AssertionError(f"central {self.process.pid} closed its output")
        return value

    def wait_ready(self):
        state, writer, reader = self.reply().split()
        assert state == "READY" and writer != reader
        print(f"central {self.process.pid} ready with Publisher {writer} and Subscriber {reader}", flush=True)

    def exchange_until_ready(self, peer, sequence):
        # Keep the existing Subscriber active throughout peer startup without relying on a sleep.
        end = time.monotonic() + TIMEOUT
        while True:
            sequence += 1
            self.exchange(sequence)
            if not peer.replies.empty():
                peer.wait_ready()
                return sequence
            assert time.monotonic() < end, "replacement central did not become ready"

    def command(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()
        return int(self.reply())

    def exchange(self, sequence):
        assert self.command(f"push {sequence}") == 0
        assert self.command("last") == sequence, f"subscriber missed {sequence}"

    def crash(self):
        self.process.kill()
        assert self.process.wait(TIMEOUT) != 0

    def stop(self):
        self.process.stdin.write("stop\n")
        self.process.stdin.flush()
        assert self.process.wait(TIMEOUT) == 0

    def close(self):
        try:
            if self.process.poll() is None:
                self.stop()
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(TIMEOUT)
            self.process.stdin.close()
            self.reader.join(TIMEOUT)
            self.process.stdout.close()


def epoch_for(epochs, app_name):
    matches = [p.parent for p in epochs.glob("*/owner") if p.read_text().strip() == app_name]
    assert len(matches) == 1, (app_name, matches)
    return matches[0]


def reservations(epoch):
    return {p.name for p in epoch.iterdir() if p.is_dir()}


def check_removed(app, names):
    # Windows objects can outlive their central until the old Hosts release the last handles.
    end = time.monotonic() + TIMEOUT
    pending = set(names)
    while pending:
        pending = {name for name in pending if app.command("probe " + name)}
        if pending:
            assert time.monotonic() < end, f"old native names still reachable: {sorted(pending)}"
            time.sleep(POLL_SECONDS)


def run(root, executable, plugin):
    state = "open-vinc-state-v1" if os.name == "nt" else ".open-vinc-state-v1"
    epochs = root / state / "epochs"
    with ExitStack() as stack:
        b = App(stack, executable, plugin, "recovery-b", root)
        b.wait_ready()
        a = App(stack, executable, plugin, "recovery-a", root)
        a.wait_ready()
        a.exchange(101)
        b.exchange(201)
        assert a.command("last") == 101
        old = epoch_for(epochs, "recovery-a")
        other = epoch_for(epochs, "recovery-b")
        old_names = reservations(old)
        other_names = reservations(other)
        assert old_names.isdisjoint(other_names)
        for names in (old_names, other_names):
            assert {name[0] for name in names} == {"n", "r"}
            assert all(b.command("probe " + name) == 1 for name in names)

        a.crash()
        assert reservations(old) == old_names
        if os.name == "posix":
            assert any(b.command("probe " + name) for name in old_names if name.startswith("r"))
        b.exchange(202)
        replacement = App(stack, executable, plugin, "recovery-a", root)
        sequence = b.exchange_until_ready(replacement, 202)
        assert replacement.command("last") == NO_PUBLISHED_DATA
        replacement.exchange(102)
        assert b.command("last") == sequence
        b.exchange(sequence + 1)
        assert replacement.command("last") == 102

        fresh = epoch_for(epochs, "recovery-a")
        assert fresh != old and not list(old.iterdir())
        assert reservations(fresh).isdisjoint(old_names | other_names)
        check_removed(b, old_names)
        assert epoch_for(epochs, "recovery-b") == other
        assert reservations(other) == other_names
        assert all(b.command("probe " + name) == 1 for name in other_names)
        replacement.stop()
        b.exchange(sequence + 2)
        b.stop()
        assert not reservations(fresh) and not reservations(other)
        print(f"A recovered {len(old_names)} old names. B retained {len(other_names)} names "
              f"and delivered sequences 201 through {sequence + 2}.")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="vinc-bus-restart-") as directory:
        run(Path(directory), *map(os.path.abspath, sys.argv[1:]))

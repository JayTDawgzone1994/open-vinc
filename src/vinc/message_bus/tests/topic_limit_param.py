"""Exercise live Topic admission, persistence and isolation through two real centrals."""

from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile

from app_restart_isolation import App


SETTING = "vinc.message_bus_max_topics"
CREATED = 0
TOPIC_CAPACITY_REACHED = 1  # Actor verifies the exact deepest framework error.
SUBSCRIBER_ONLY_TOPICS = 141


def expect(app, command, expected=CREATED):
    actual = app.command(command)
    assert actual == expected, f"{command}: expected {expected}, got {actual}"


def run(root, executable, plugin):
    with ExitStack() as stack:
        a = App(stack, executable, plugin, "limit-a", root, "default")
        a.wait_ready()
        b = App(stack, executable, plugin, "limit-b", root, "2")
        b.wait_ready()

        # Each App already has a Publisher and Subscriber on recovery/shared.
        for index in range(SUBSCRIBER_ONLY_TOPICS):
            expect(a, f"module hold-subscription limit/idle{index}")
        expect(b, "module hold-subscription limit/b")
        expect(b, "module probe-subscription limit/new", TOPIC_CAPACITY_REACHED)

        expect(a, f"param set {SETTING} 2")
        expect(a, "module probe-subscription limit/new", TOPIC_CAPACITY_REACHED)
        expect(a, "module probe-explicit-publisher limit/new", TOPIC_CAPACITY_REACHED)
        expect(a, "module probe-subscription limit/idle0")
        expect(a, "module probe-explicit-publisher limit/idle0")
        a.exchange(101)
        for _ in range(SUBSCRIBER_ONLY_TOPICS - 1):
            expect(a, "module release-subscription")
        expect(a, "module probe-subscription limit/second", TOPIC_CAPACITY_REACHED)
        expect(a, "module release-subscription")
        expect(a, "module hold-subscription limit/second")
        expect(a, "module probe-subscription limit/third", TOPIC_CAPACITY_REACHED)
        expect(a, f"param set {SETTING} 3")
        expect(a, "module hold-subscription limit/third")
        expect(a, "module probe-subscription limit/fourth", TOPIC_CAPACITY_REACHED)
        expect(a, "param save")

        expect(a, f"param set {SETTING} 0")
        for index in range(SUBSCRIBER_ONLY_TOPICS - 2):
            expect(a, f"module hold-subscription burst/topic{index}")
        expect(a, f"param set {SETTING} 2")
        # All 141 destructors run in one callback. The next fresh name fits only after cleanup.
        expect(a, "module clear-and-subscribe burst/replacement")
        a.exchange(303)
        expect(a, "module hold-subscription limit/second")
        expect(a, "module probe-subscription limit/third", TOPIC_CAPACITY_REACHED)

        expect(a, f"param set {SETTING} 0")
        expect(a, "module hold-subscription limit/fourth")
        expect(b, "module probe-subscription limit/fourth", TOPIC_CAPACITY_REACHED)
        a.stop()
        restarted = App(stack, executable, plugin, "limit-a", root, "default")
        restarted.wait_ready()
        expect(restarted, "module hold-subscription limit/second")
        expect(restarted, "module hold-subscription limit/third")
        expect(restarted, "module probe-subscription limit/fourth", TOPIC_CAPACITY_REACHED)
        b.exchange(202)
        expect(b, "module probe-subscription limit/third", TOPIC_CAPACITY_REACHED)
        print("142 live topics; live shrink, raise, disable, saved restart and independent App passed.")


def exercise_routes(root, executable, plugin):
    with ExitStack() as stack:
        app = App(stack, executable, plugin, "release-routes", root, "default")
        app.wait_ready()
        for index in range(SUBSCRIBER_ONLY_TOPICS):
            topic = f"routes/topic{index}"
            expect(app, f"writer hold-publisher {topic}")
            expect(app, f"module hold-callback {topic}")
        # Every removed callback changes the remote writer's notification routes.
        expect(app, "module clear-and-subscribe routes/replacement")
        app.exchange(301)
        for index in range(SUBSCRIBER_ONLY_TOPICS):
            expect(app, f"module hold-callback routes/topic{index}")
        # Reverse direction: Detach must reach the reader for every retired ring.
        expect(app, "writer clear-publishers")
        expect(app, "module clear-and-subscribe routes/replacement")
        app.exchange(302)
        app.stop()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="vinc-topic-limit-") as directory:
        action = exercise_routes if sys.argv[1] == "--routes" else run
        executable, plugin = sys.argv[-2:]
        action(Path(directory), str(Path(executable).resolve()), str(Path(plugin).resolve()))

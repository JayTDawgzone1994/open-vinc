import gc
import threading
import time
import vinc

TIMEOUT_SECONDS = 8
POLL_SECONDS = 0.01
MESSAGE_LIMIT_BYTES = 4096
PARALLEL_READ_COUNT = 64
# Published numeric identities; Python exposes Error fields without component enums.
PARAM_ERROR_DOMAIN = 10
PARAM_PERMISSION_DENIED = 6


def raises(kind, function):
    try:
        function()
    except kind:
        return
    raise AssertionError(f"expected {kind.__name__}")


class Module(vinc.BaseModule, mode=vinc.RunMode.Manual):
    def on_start(self, args):
        assert vinc.format_utc(0) == "1970-01-01T00:00:00.000000000Z"
        assert vinc.format_utc(2**64 - 1) == "2554-07-21T23:34:33.709551615Z"
        raises(TypeError, lambda: vinc.format_utc(-1))
        raises(TypeError, lambda: vinc.format_utc(2**64))
        self.mode = args[0] if args else "normal"
        self.expected_publisher_id = int(args[1])
        self.received = []
        self.snapshots = []
        self.pump_thread = threading.get_ident()
        options = vinc.message_bus.PublisherOptions()
        options.slots = 8
        options.max_message_bytes = MESSAGE_LIMIT_BYTES
        self.publisher = self.context.advertise(vinc.messages.int3, "python/request", options)
        self.subscriber = self.context.subscribe(vinc.messages.int3, "python/reply", self.received_message)
        self.context.param.register_param("gain", vinc.param.Type.Double, 1.25, vinc.param.Permission.Public)
        self.context.param.register_param("protected", vinc.param.Type.Int32, 7,
                                          restart_hint=vinc.param.RestartHint.App)
        assert self.context.param.read_current("protected").restart_hint == vinc.param.RestartHint.App
        raises(vinc.VincError, lambda: self.context.param.register_param(
            "nativeNode.fixed", vinc.param.Type.Int32, restart_hint=vinc.param.RestartHint.Module))
        # Foreign reads do not require notification subscriptions.
        watches_before = len(self.context.param.watches())
        assert self.context.param.read_current("nativeNode.fixed").value == 7
        assert self.context.param.read_current("nativeNode.fixed").restart_hint == vinc.param.RestartHint.Module
        assert len(self.context.param.watches()) == watches_before
        self.context.param.register_param("missing.value", vinc.param.Type.Int32)
        self.context.param.register_param("nativeNode.fixed", vinc.param.Type.Int32)
        self.context.param.register_param("nativeNode.shared", vinc.param.Type.Int32)
        self.context.param.on_change(self.params_changed)
        current = self.context.param.read_current("gain")
        assert current.value == 1.25 and current.default_value == 1.25
        assert current.type == vinc.param.Type.Double
        assert current.permission == vinc.param.Permission.Public
        assert current.restart_hint == vinc.param.RestartHint.None_
        assert current.version > 0
        assert self.snapshots == []
        raises(vinc.VincError, lambda: self.context.param.update("gain", vinc.param.Type.Double))
        raises(vinc.VincError, lambda: self.context.param.read_current("missing.value"))
        raises(vinc.VincError, lambda: self.context.param.read_current("bad/name"))
        self.startup_entry = current
        self.context.log.info("PYTHON_STRUCTURED_LOG")
        self.context.log.warn("Python warning")
        self.context.log.error("Python error level")
        self.context.log.debug("Python debug level")

    def params_changed(self, snapshot):
        assert threading.get_ident() == self.pump_thread
        if self.mode == "param-error":
            raise ValueError("PYTHON_PARAM_CALLBACK_FAILURE")
        self.snapshots.append(snapshot)
        assert self.context.param.read_current("gain").version >= snapshot["pythonNode.gain"].version
        assert "missing.value" not in snapshot
        assert snapshot["pythonNode.gain"].type == vinc.param.Type.Double
        try:
            snapshot["illegal"] = 1
        except TypeError:
            pass
        else:
            raise AssertionError("snapshot must be read-only")

    def received_message(self, message):
        assert threading.get_ident() == self.pump_thread
        if self.mode == "bus-error":
            raise ValueError("PYTHON_BUS_CALLBACK_FAILURE")
        if self.mode == "vinc-error":
            self.context.pumpOnce()
        assert message.publisher_name() == "nativeNode"
        assert message.topic_name() == "python/reply"
        # Publisher IDs and Module MIDs are independent namespaces and may coincide.
        assert message.publisher_instance() == self.expected_publisher_id, "Unexpected reply Publisher ID"
        if self.mode == "same-id":
            assert message.publisher_instance() == self.get_mid(), "ID overlap fixture was not exercised"
        assert message.get_topic_count() > 0
        assert message.get_message_timestamp() > 1_000_000_000_000_000_000
        assert message.get_message_monotonic_timestamp() > 0
        assert vinc.format_utc(message.get_message_timestamp()).endswith("Z")
        copied = message.copy()
        assert copied.publisher_instance() == self.expected_publisher_id
        assert copied.publisher_name() == "nativeNode"
        assert copied.get_message_timestamp() == message.get_message_timestamp()
        assert copied.get_message_monotonic_timestamp() == message.get_message_monotonic_timestamp()
        assert message.data().x == 42
        assert message.data().y == 84
        self.received.append(message)

    def check_parallel_reads(self):
        failures = []
        done = threading.Event()

        def read_values():
            try:
                for _ in range(PARALLEL_READ_COUNT):
                    assert self.context.param.read_current("gain").value == 2.5
            except BaseException as error:
                failures.append(error)
            finally:
                done.set()

        reader = threading.Thread(target=read_values)
        reader.start()
        try:
            # Host admission is shared with pump; native waits release the GIL.
            self.wait(done.is_set)
        finally:
            reader.join(TIMEOUT_SECONDS)
        assert not reader.is_alive(), "concurrent Param reader did not stop"
        assert not failures, failures

    def wait(self, predicate):
        deadline = time.monotonic() + TIMEOUT_SECONDS
        while self.context.living() and not predicate():
            assert time.monotonic() < deadline, "Python service fixture timed out"
            self.context.pumpOnce()
            time.sleep(POLL_SECONDS)

    def main(self):
        self.wait(lambda: bool(self.snapshots))
        if self.mode == "param-error":
            return
        assert self.context.param.update("gain", vinc.param.Type.Double) == 1.25
        assert any(not watch.present for watch in self.context.param.watches())
        raises(vinc.VincError, lambda: self.context.param.update("missing.value", vinc.param.Type.Int32))
        self.context.param.set("protected", vinc.param.Type.Int32, 9)
        self.wait(lambda: self.context.param.update("protected", vinc.param.Type.Int32) == 9)
        assert self.snapshots[-1]["pythonNode.protected"].permission == vinc.param.Permission.Limited
        assert self.snapshots[-1]["pythonNode.protected"].restart_hint == vinc.param.RestartHint.App
        assert self.snapshots[-1]["nativeNode.fixed"].restart_hint == vinc.param.RestartHint.Module
        assert self.snapshots[-1]["pythonNode.gain"].permission == vinc.param.Permission.Public
        assert not hasattr(vinc.param.Permission, "FrameworkOnly")
        assert not hasattr(vinc.param.Permission, "Open")
        assert self.context.param.update("nativeNode.fixed", vinc.param.Type.Int32) == 7
        try:
            self.context.param.set("nativeNode.fixed", vinc.param.Type.Int32, 9)
        except vinc.VincError as error:
            # Param's published PermissionDenied code; do not accept NotFound as protection.
            assert error.error.domain == PARAM_ERROR_DOMAIN
            assert error.error.code == PARAM_PERMISSION_DENIED
        else:
            raise AssertionError("Python must not write a foreign Limited parameter")
        self.context.param.set("nativeNode.shared", vinc.param.Type.Int32, 9)
        self.wait(lambda: self.context.param.update("nativeNode.shared", vinc.param.Type.Int32) == 9)
        self.context.param.set("gain", vinc.param.Type.Double, 2.5)
        callbacks_before = len(self.snapshots)
        assert self.context.param.read_current("gain").value == 2.5
        assert self.startup_entry.value == 1.25
        assert self.context.param.update("gain", vinc.param.Type.Double) == 1.25
        assert len(self.snapshots) == callbacks_before
        self.wait(lambda: self.context.param.update("gain", vinc.param.Type.Double) == 2.5)
        assert self.snapshots[0]["pythonNode.gain"].value == 1.25
        assert self.snapshots[-1]["pythonNode.gain"].value == 2.5
        self.check_parallel_reads()
        self.context.param.on_change(None)
        local = vinc.make_message(vinc.messages.int3)
        local.data().x = 42
        self.publisher.push(local)
        assert local.publisher_instance() == 0
        loan = self.publisher.get_next()
        saved = loan.data()
        saved.x = 42
        self.publisher.commit(loan)
        raises(ReferenceError, lambda: saved.x)
        raises(ReferenceError, loan.data)
        raises(ReferenceError, lambda: self.publisher.commit(loan))
        self.wait(lambda: len(self.received) >= 2)
        if self.mode not in ("normal", "same-id"):
            return
        retained = self.received[0]
        assert retained.copy().publisher_instance() == retained.publisher_instance()
        latest = self.subscriber.get_last()
        assert latest.publisher_instance() == self.expected_publisher_id
        assert latest.publisher_name() == "nativeNode"
        assert latest.data().y == 84
        latest.data().y = 999
        assert self.subscriber.get_last().data().y == 84
        assert retained.data().y == 84
        assert self.publisher.stats().published == 2
        assert self.subscriber.stats().lost_messages == 0
        assert any(topic.name == "python/request" for topic in self.context.list_topics().topics)
        self.check_loans()
        self.subscriber.close()
        self.publisher.close()
        print("PYTHON_SERVICES_PASSED")

    def check_loans(self):
        default_publisher = self.context.advertise(vinc.messages.test, "python/defaults")
        assert default_publisher.stats().requested_slots == 5
        assert default_publisher.stats().max_message_bytes == 4096
        default_publisher.close()
        options = vinc.message_bus.PublisherOptions()
        assert options.slots is None and options.max_message_bytes is None
        options.max_message_bytes = MESSAGE_LIMIT_BYTES
        publisher = self.context.advertise(vinc.messages.test, "python/loans", options)
        assert publisher.stats().requested_slots == self.context.param.read_current("vinc.pub_default_slots").value
        assert publisher.stats().max_message_bytes == MESSAGE_LIMIT_BYTES
        options.slots = 0
        raises(vinc.VincError, lambda: self.context.advertise(vinc.messages.test, "python/zero", options))
        options.slots = None
        loan = publisher.get_next()
        array = loan.data().y
        array.assign([1, 2, 3])
        raises(vinc.VincError, lambda: array.resize(MESSAGE_LIMIT_BYTES))
        assert list(array) == [1, 2, 3]
        raises(vinc.VincError, publisher.get_next)
        publisher.commit(loan)
        raises(ReferenceError, lambda: len(array))
        discarded = publisher.get_next()
        del discarded
        gc.collect()
        raises(vinc.VincError, publisher.get_next)
        publisher.close()
        closing = self.context.advertise(vinc.messages.test, "python/closed", options)
        loan = closing.get_next()
        stale = loan.data().y
        closing.close()
        raises(ReferenceError, lambda: len(stale))
        raises(ReferenceError, loan.data)
        oversized = vinc.make_message(vinc.messages.test)
        oversized.data().y.resize(MESSAGE_LIMIT_BYTES)
        checked = self.context.advertise(vinc.messages.test, "python/oversized", options)
        raises(vinc.VincError, lambda: checked.push(oversized))
        small = checked.get_next()
        checked.commit(small)
        checked.close()
        empty = self.context.subscribe(vinc.messages.test, "python/absent")
        raises(vinc.VincError, empty.get_last)
        empty.close()

    def on_stop(self):
        print("PYTHON_SERVICES_STOPPED")

    def __del__(self):
        try:
            self.context.get_mid()
        except vinc.VincError:
            print("PYTHON_SERVICE_REFERENCES_RELEASED")

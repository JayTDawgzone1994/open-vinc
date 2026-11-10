"""Controlled Python dispatch/lifetime fixture; gates release the GIL."""
import pathlib
import threading
import time
import vinc

GATE_SECONDS = 8
FILE_POLL_SECONDS = 0.002
SAMPLES = 4
MODULE_DOMAIN = 3
RPC_DOMAIN = 14
BUSY = 18
BLOCKING_CALL_IN_PUMP = 18


class RetiredCallback:
    def __init__(self, context, path):
        self.context = context
        self.path = path

    def __call__(self, message):
        raise AssertionError("closed subscription dispatched")

    def __del__(self):
        # Native callback retirement must acquire the GIL, including worker exits.
        self.path.write_text(str(self.context.get_mid()), encoding="utf-8")


class Behavior:
    def on_start(self, args):
        self.case = args[0]
        self.root = pathlib.Path(args[1])
        self.lock = threading.Lock()
        self.entered = threading.Event()
        self.other = threading.Event()
        self.control = threading.Event()
        self.param_seen = threading.Event()
        self.rpc_seen = threading.Event()
        self.active = set()
        self.received = {"a": [], "duplicate": [], "b": []}
        self.thread = None
        self.thread_error = None
        self.lifecycle_thread = threading.get_ident()
        self.control_thread = None
        self.lifecycle_cases = ("return", "fault", "stall")
        options = vinc.message_bus.PublisherOptions()
        options.slots = 16
        options.max_message_bytes = 4096
        self.publisher = self.context.advertise(vinc.messages.int3, "python/request", options)
        self.local = self.context.advertise(vinc.messages.int3, "parallel/second", options)
        if self.case == "idle":
            return
        if self.case in self.lifecycle_cases:
            self.sub = self.context.subscribe(vinc.messages.int3, "parallel/second", self.drain)
            self.local.push(vinc.make_message(vinc.messages.int3))
            return

        self.sub = self.context.subscribe(vinc.messages.int3, "python/reply", lambda msg: self.receive("a", msg))
        self.duplicate = self.context.subscribe(vinc.messages.int3, "python/reply",
                                                lambda msg: self.receive("duplicate", msg))
        self.second = self.context.subscribe(vinc.messages.int3, "parallel/second", lambda msg: self.receive("b", msg))
        self.context.param.register_param("gain", vinc.param.Type.Int32, 7)
        self.context.param.on_change(self.params_changed)
        self.context.register_rpc(vinc.messages.int3, vinc.messages.int3, "echo", self.echo)
        self.client = self.context.rpc_client(vinc.messages.int3, vinc.messages.int3,
                                              "pythonNode.echo", self.completed)
        for index in range(SAMPLES):
            message = vinc.make_message(vinc.messages.int3)
            message.data().x = 42
            message.data().z = index
            self.publisher.push(message)
            self.local.push(message)

    def check_guard(self):
        for action in (self.context.pumpOnce, self.context.waitForWork, self.context.pumpLoop):
            try:
                action()
            except vinc.VincError as error:
                assert (error.error.domain, error.error.code) == (MODULE_DOMAIN, BUSY)
            else:
                raise AssertionError("recursive pump/wait was admitted")
        pair = vinc.make_rpc_messages(vinc.messages.int3, vinc.messages.int3)
        try:
            self.context.call_rpc("missing.echo", pair)
        except vinc.VincError as error:
            assert (error.error.domain, error.error.code) == (RPC_DOMAIN, BLOCKING_CALL_IN_PUMP)
        else:
            raise AssertionError("blocking callback call was admitted")

    def check_control(self):
        current = threading.get_ident()
        if self.control_thread is None:
            self.control_thread = current
        assert self.control_thread == current
        self.check_guard()

    def params_changed(self, values):
        self.check_control()
        assert values["pythonNode.gain"].value == 7
        self.param_seen.set()

    def echo(self, request, response):
        self.check_control()
        response.data().x = request.data().x
        return vinc.rpc.RpcStatus.success()

    def completed(self, reply, error):
        self.check_control()
        assert error is None, error
        status, message = reply
        assert status.code == 0 and message.data().x == 42
        self.rpc_seen.set()

    def receive(self, lane, message):
        topic = message.topic_name()
        with self.lock:
            assert topic not in self.active, "same-topic callbacks overlapped"
            self.active.add(topic)
            first = not self.received[lane]
        try:
            assert threading.get_ident() != self.control_thread
            self.check_guard()
            if first and lane == "a":
                self.entered.set()
                assert self.other.wait(GATE_SECONDS), "second Topic could not enter"
                request = self.client.make_request()
                request.data().x = 42
                self.client.call(request)
                assert self.param_seen.wait(GATE_SECONDS)
                assert self.rpc_seen.wait(GATE_SECONDS)
                assert self.control.wait(GATE_SECONDS), "Shell could not overlap Topic"
                # Registration and close while another worker is active, followed
                # by callback destruction, exercise GIL-safe native retirement.
                temporary = self.context.subscribe(vinc.messages.int3, "parallel/retired",
                    RetiredCallback(self.context, self.root / "retired"))
                temporary.close()
            elif first and lane == "b":
                self.other.set()
                assert self.entered.wait(GATE_SECONDS)
            assert message.data().z == len(self.received[lane])
            if lane != "b":
                assert message.publisher_name() == "nativeNode"
                assert message.data().y == 84
            with self.lock:
                # Retain the delivered ownership, without message.copy(). Later
                # callbacks must never overwrite these writable local objects.
                self.received[lane].append(message)
        finally:
            with self.lock:
                self.active.remove(topic)

    def drain(self, message):
        with self.lock:
            self.active.add(message.topic_name())
        self.entered.set()
        (self.root / "entered").write_text("ready", encoding="utf-8")
        deadline = time.monotonic() + GATE_SECONDS
        try:
            while not (self.root / "release").exists():
                assert time.monotonic() < deadline, "test gate timed out"
                time.sleep(FILE_POLL_SECONDS)
            if self.case == "fault":
                raise ValueError("PARALLEL_PYTHON_CALLBACK_FAULT")
            self.received["b"].append(message)
        finally:
            with self.lock:
                self.active.clear()

    def main(self):
        if self.case not in self.lifecycle_cases:
            self.context.pumpLoop(vinc.PumpOptions(topic_workers=2))
            return

        def pump():
            try:
                self.context.pumpLoop(vinc.PumpOptions(topic_workers=2))
            except vinc.VincError as error:
                self.thread_error = error

        self.thread = threading.Thread(target=pump)
        self.thread.start()
        assert self.entered.wait(GATE_SECONDS)
        # Return with an active callback. Runtime must release the GIL while
        # quiescing, before on_stop may join this application-owned thread.

    def module_interface(self, args):
        self.check_control()
        if self.entered.is_set() and self.other.is_set():
            self.control.set()
        with self.lock:
            return 42 if all(len(values) == SAMPLES for values in self.received.values()) else 1

    def on_stop(self):
        assert threading.get_ident() == self.lifecycle_thread
        assert not self.active, "on_stop preceded callback drain"
        (self.root / "stop_entered").write_text("ready", encoding="utf-8")
        if self.thread:
            self.thread.join(GATE_SECONDS)
            assert not self.thread.is_alive(), "GIL-held join prevented pump return"
        if self.case not in (*self.lifecycle_cases, "idle"):
            assert (self.root / "retired").exists()
            for values in self.received.values():
                assert [msg.data().z for msg in values] == list(range(SAMPLES))
            self.received["a"][0].data().z = 999
            assert self.received["duplicate"][0].data().z == 0
            assert self.received["a"][1].data().z == 1
            assert self.sub.stats().lost_messages == 0
        (self.root / "cleaned").write_text("done", encoding="utf-8")

    def __del__(self):
        # Bound callback cycles are cleared during GIL-held teardown, not left
        # for interpreter finalization (the Module process uses os._exit).
        if hasattr(self, "root"):
            try:
                self.context.get_mid()
            except vinc.VincError:
                (self.root / "destroyed").write_text("revoked", encoding="utf-8")

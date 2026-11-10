import os
import threading
import vinc
from helpers import greeting

# Published Param error identities; reject permission failures specifically.
PARAM_ERROR_DOMAIN = 10
PARAM_NOT_FOUND = 4
PARAM_PERMISSION_DENIED = 6


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop,
             pump_options=vinc.PumpOptions(topic_workers=2)):
    def on_start(self, args):
        self.context.waitForWork(timeout=0)
        self.context.waitForWork(timeout=0.001)
        print("INSTALLED_PYTHON_WAIT_OK", flush=True)
        self.rpc_client = self.context.rpc_client(vinc.messages.SdkPacket, vinc.messages.SdkPacket,
                                                  "native.echo", self.completed)
        print(greeting(), flush=True)
        print(f"PYTHON_PID={os.getpid()}", flush=True)
        self.revision = int(args[0])
        self.context.register_rpc(vinc.messages.SdkPacket, vinc.messages.SdkPacket, "echo", self.echo)
        messages = vinc.make_rpc_messages(vinc.messages.SdkPacket, vinc.messages.SdkPacket)
        messages.request().data().value = self.revision
        status = self.context.call_rpc("native.echo", messages)
        assert status.code == 0 and messages.response().data().value == self.revision + 2
        print("INSTALLED_PYTHON_SYNC_RPC_OK", flush=True)
        self.lock = threading.Lock()
        self.arrived = [threading.Event(), threading.Event()]
        self.callback_threads = [None, None]
        self.retained = [None, None]
        self.rpc_received = False
        self.changed = False
        self.publisher = self.context.advertise(vinc.messages.SdkPacket, "sdk/request")
        self.second_publisher = self.context.advertise(vinc.messages.SdkPacket, "sdk/request2")
        assert self.publisher.stats().requested_slots == 4
        assert self.publisher.stats().max_message_bytes == 8192
        options = vinc.message_bus.PublisherOptions()
        assert options.slots is None and options.max_message_bytes is None
        options.max_message_bytes = 4096
        partial = self.context.advertise(vinc.messages.SdkPacket, "sdk/partial", options)
        assert partial.stats().requested_slots == 4
        assert partial.stats().max_message_bytes == 4096
        partial.close()
        print("INSTALLED_PYTHON_PUBLISHER_DEFAULTS_OK", flush=True)
        self.subscriber = self.context.subscribe(vinc.messages.SdkPacket, "sdk/reply",
                                                 lambda message: self.receive(0, message))
        self.second_subscriber = self.context.subscribe(vinc.messages.SdkPacket, "sdk/reply2",
                                                        lambda message: self.receive(1, message))
        self.context.param.register_param("gain", vinc.param.Type.Int32, 7,
                                         restart_hint=vinc.param.RestartHint.App)
        self.context.param.register_param("plain", vinc.param.Type.Int32, 1)
        plain = self.context.param.read_current("plain")
        assert plain.permission == vinc.param.Permission.Limited
        assert plain.restart_hint == vinc.param.RestartHint.None_
        # No pump has run: current reads neither subscribe nor refresh the cache.
        self.context.param.set("gain", vinc.param.Type.Int32, 7)
        initial = self.context.param.read_current("gain")
        assert initial.value == 7
        assert initial.default_value == 7
        assert initial.permission == vinc.param.Permission.Limited
        assert initial.restart_hint == vinc.param.RestartHint.App
        watches_before = len(self.context.param.watches())
        native = self.context.param.read_current("native.fixed")
        assert native.value == 7
        assert native.restart_hint == vinc.param.RestartHint.Module
        assert len(self.context.param.watches()) == watches_before
        try:
            self.context.param.update("gain", vinc.param.Type.Int32)
        except vinc.VincError as error:
            assert error.error.domain == PARAM_ERROR_DOMAIN
            assert error.error.code == PARAM_NOT_FOUND
        else:
            raise AssertionError("read_current must not populate the pump cache")
        try:
            self.context.param.set("native.fixed", vinc.param.Type.Int32, 9)
        except vinc.VincError as error:
            assert error.error.domain == PARAM_ERROR_DOMAIN
            assert error.error.code == PARAM_PERMISSION_DENIED
        else:
            raise AssertionError("foreign Limited writes must be rejected")
        self.context.param.set("native.shared", vinc.param.Type.Int32, 9)
        shared = self.context.param.read_current("native.shared")
        assert shared.value == 9
        assert shared.permission == vinc.param.Permission.Public
        assert shared.restart_hint == vinc.param.RestartHint.None_
        self.context.param.set("gain", vinc.param.Type.Int32, 8)
        assert self.context.param.read_current("gain").value == 8
        assert initial.value == 7
        print("PYTHON_PARAM_CURRENT_OK", flush=True)
        self.context.param.on_change(self.changed_param)
        self.context.log.info("INSTALLED_PYTHON_LOG")

    def changed_param(self, snapshot):
        assert snapshot["py.gain"].restart_hint == vinc.param.RestartHint.App
        self.changed = snapshot["py.gain"].value == 8

    def receive(self, index, message):
        try:
            self.context.waitForWork()
        except vinc.VincError as error:
            assert error.error.domain == 3 and error.error.code == 18
            assert error.error.operation == 71
        else:
            raise AssertionError("subscription callback must reject nested wait")
        assert message.publisher_name() == "native"
        assert message.data().value == self.revision + 1
        # Event.wait releases the GIL: this proves native worker overlap without
        # claiming parallel execution of pure Python CPU instructions.
        with self.lock:
            self.callback_threads[index] = threading.get_ident()
        self.arrived[index].set()
        assert self.arrived[1 - index].wait(timeout=5), "installed Python worker gate timed out"
        with self.lock:
            assert self.callback_threads[0] != self.callback_threads[1]
            self.retained[index] = message

    def on_stop(self):
        assert all(message is not None for message in self.retained)
        assert self.retained[0] is not self.retained[1]
        self.retained[0].data().value = 0
        assert self.retained[1].data().value == self.revision + 1
        print("INSTALLED_PYTHON_PARALLEL_OK", flush=True)

    def completed(self, reply, error):
        assert error is None, error
        status, response = reply
        assert status.code == 0
        assert response.data().value == self.revision + 2
        self.rpc_received = True

    def echo(self, request, response):
        response.data().value = request.data().value + 3
        return vinc.rpc.RpcStatus.success()

    def module_interface(self, args):
        if args[-1] == "send":
            message = vinc.make_message(vinc.messages.SdkPacket)
            message.data().value = self.revision
            self.publisher.push(message)
            self.second_publisher.push(message)
            self.rpc_client.call(message)
            return 0
        with self.lock:
            received = all(message is not None for message in self.retained)
        if received and self.rpc_received and self.changed:
            assert self.context.param.update("gain", vinc.param.Type.Int32) == 8
            print("PYTHON_PARAM_RESTART_OK", flush=True)
            print(f"PYTHON_FLOW_REVISION={self.revision}", flush=True)
            return 0
        return 1

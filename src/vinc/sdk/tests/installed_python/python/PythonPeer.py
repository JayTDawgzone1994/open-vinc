import os
import vinc


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop):
    def on_start(self, args):
        print(f"PYTHON_PID={os.getpid()}", flush=True)
        self.revision = int(args[0])
        self.received = False
        self.rpc_received = False
        self.changed = False
        self.publisher = self.context.advertise(vinc.messages.SdkPacket, "sdk/request")
        self.subscriber = self.context.subscribe(vinc.messages.SdkPacket, "sdk/reply", self.receive)
        self.context.param.register_param("gain", vinc.param.Type.Int32, 7)
        self.context.param.on_change(self.changed_param)
        self.context.log.info("INSTALLED_PYTHON_LOG")

    def changed_param(self, snapshot):
        self.changed = snapshot["py.gain"].value == 7

    def receive(self, message):
        assert message.publisher_name() == "native"
        assert message.data().value == self.revision + 1
        self.received = True

    def completed(self, reply, error):
        assert error is None, error
        status, response = reply
        assert status.code == 0
        assert response.data().value == self.revision + 2
        self.rpc_received = True

    def module_interface(self, args):
        if args[-1] == "send":
            message = vinc.make_message(vinc.messages.SdkPacket)
            message.data().value = self.revision
            self.publisher.push(message)
            self.context.call_rpc(vinc.messages.SdkPacket, vinc.messages.SdkPacket,
                                  "native", "echo", message, self.completed)
            return 0
        if self.received and self.rpc_received and self.changed:
            assert self.context.param.update("gain", vinc.param.Type.Int32) == 7
            print(f"PYTHON_FLOW_REVISION={self.revision}", flush=True)
            return 0
        return 1

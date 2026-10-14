import time
import vinc


class Module(vinc.BaseModule, mode=vinc.RunMode.Manual):
    def on_start(self, args):
        self.done = False
        self.served = False
        self.context.register_rpc(vinc.messages.int3, vinc.messages.int3, "echo", self.echo)

    def echo(self, request, response):
        assert request.data().x == 42
        response.data().x = 44
        self.served = True
        return vinc.rpc.RpcStatus.success()

    def completed(self, reply, error):
        if error is not None:
            raise error
        status, message = reply
        assert status.code == 0
        assert message.data().x == 43
        self.done = True

    def main(self):
        request = vinc.make_message(vinc.messages.int3)
        request.data().x = 42
        self.context.call_rpc(vinc.messages.int3, vinc.messages.int3,
                              "rpcNative", "echo", request, self.completed)
        request.data().x = 99
        deadline = time.monotonic() + 10
        while not (self.done and self.served):
            assert self.context.living() and time.monotonic() < deadline
            self.context.pumpOnce()
            time.sleep(0.005)
        # Continue pumping until central explicitly stops this fixture.
        print("RPC_PYTHON_PASSED")
        self.context.pumpLoop()

    def module_interface(self, args):
        return int(self.done and self.served)

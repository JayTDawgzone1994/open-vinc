import time
import threading
import vinc

POLL_SECONDS = 0.005
TEST_SECONDS = 10
TIMEOUT_MS = 1
TIMEOUT_OBSERVATION_SECONDS = 0.02


class Module(vinc.BaseModule, mode=vinc.RunMode.Manual):
    def on_start(self, args):
        self.fail_callback = "callback-failure" in args
        self.done = False
        self.replies = []
        self.served = False
        self.context.register_rpc(vinc.messages.int3, vinc.messages.int3, "echo", self.echo)

    def echo(self, request, response):
        assert request.data().x == 42
        response.data().x = 44
        self.served = True
        return vinc.rpc.RpcStatus.success()

    def completed(self, reply, error):
        if self.fail_callback:
            raise RuntimeError("intentional reusable RPC callback failure")
        if error is not None:
            raise error
        status, message = reply
        assert status.code == 0
        assert message.data().x == 43
        self.replies.append(message)
        pair = vinc.make_rpc_messages(vinc.messages.int3, vinc.messages.int3)
        try:
            self.context.call_rpc("rpcNative.sync", pair)
        except vinc.VincError as blocked:
            assert blocked.error.code == 18
        else:
            raise AssertionError("pump must reject synchronous calls")
        if len(self.replies) == 1:
            request = self.client.make_request()
            request.data().x = 42
            self.client.call(request)
        else:
            assert len(self.replies) == 2
            self.client.close()  # Closing during dispatch must not destroy the active callable.
            self.done = True

    def main(self):
        pair = vinc.make_rpc_messages(vinc.messages.int3, vinc.messages.int3)
        pair.request().data().x = 40
        ticks = []
        stop = threading.Event()
        ready = threading.Event()

        def ticker():
            ready.set()
            while not stop.wait(POLL_SECONDS):
                ticks.append(1)

        worker = threading.Thread(target=ticker)
        worker.start()
        ready.wait()
        try:
            status = self.context.call_rpc("rpcNative.sync", pair)
            assert status.code == 0 and pair.response().data().x == 42
            assert ticks, "blocking native wait must release GIL"
        finally:
            stop.set()
            worker.join()
        before = pair.response()
        try:
            self.context.call_rpc("rpcNative.missing", pair)
        except vinc.VincError:
            assert pair.response() is before
        else:
            raise AssertionError("missing service must fail")
        try:
            self.context.call_rpc("rpcPython.echo", pair)
        except vinc.VincError as blocked:
            assert blocked.error.code == 19
        else:
            raise AssertionError("synchronous self call must fail")

        closed = self.context.rpc_client(vinc.messages.int3, vinc.messages.int3,
                                         "rpcNative.sync", self.unexpected_reply)
        closed.call(pair.request())
        closed.close()
        deadline = time.monotonic() + TEST_SECONDS
        while self.context.rpc_stats().pending:
            assert time.monotonic() < deadline
            self.context.pumpOnce()
            time.sleep(POLL_SECONDS)

        outcomes = []
        timed = self.context.rpc_client(vinc.messages.int3, vinc.messages.int3,
                                        "rpcPython.echo", lambda reply, error: outcomes.append(error))
        options = vinc.rpc.CallOptions()
        options.timeout_ms = TIMEOUT_MS
        timed.call(pair.request(), options)
        # No pump means no request transfer/handler; only the timeout may fire.
        time.sleep(TIMEOUT_OBSERVATION_SECONDS)
        while not outcomes:
            assert time.monotonic() < deadline
            self.context.pumpOnce()
        assert isinstance(outcomes[0], vinc.VincError) and outcomes[0].error.code == 12
        timed.close()

        request = vinc.make_message(vinc.messages.int3)
        request.data().x = 42
        self.client = self.context.rpc_client(vinc.messages.int3, vinc.messages.int3,
                                              "rpcNative.echo", self.completed)
        self.client.call(request)
        try:
            self.client.call(request)
        except vinc.VincError as busy:
            assert busy.error.code == 17
        else:
            raise AssertionError("client must remain Busy until pump delivery")
        request.data().x = 99
        deadline = time.monotonic() + TEST_SECONDS
        while not (self.done and self.served):
            assert self.context.living() and time.monotonic() < deadline
            self.context.pumpOnce()
            time.sleep(POLL_SECONDS)
        # Continue pumping until central explicitly stops this fixture.
        print("RPC_PYTHON_PASSED")
        self.context.pumpLoop()

    def module_interface(self, args):
        return int(self.done and self.served)

    def unexpected_reply(self, reply, error):
        raise AssertionError("closed client dispatched a callback")

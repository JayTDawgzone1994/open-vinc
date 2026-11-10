import vinc


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop):
    def on_start(self, args):
        self.client = None
        self.address = None
        self.context.register_rpc(
            vinc.messages.int3, vinc.messages.int3, "echo", self.echo
        )

    def echo(self, request, response):
        response.data().x = request.data().x
        response.data().y = request.data().y
        response.data().z = request.data().z
        return vinc.rpc.RpcStatus.success()

    def completed(self, reply, error):
        if error is not None:
            # Handle recoverable call failures here; an escaping exception fails this Module.
            print(f"RPC failed: {error}")
            return

        status, response = reply
        print(f"RPC status={status.code} reason={status.reason} x={response.data().x}")

    def module_interface(self, args):
        if args and args[0] == self.get_module_name():
            args = args[1:]

        if len(args) != 1:
            print(f"usage: {self.get_module_name()} <target-module>")
            return 1

        try:
            address = args[0] + ".echo"
            if self.address != address:
                if self.client is not None:
                    self.client.close()
                self.client = self.context.rpc_client(
                    vinc.messages.int3, vinc.messages.int3, address, self.completed)
                self.address = address
            request = self.client.make_request()
            request.data().x = 42
            self.client.call(request)
        except vinc.VincError as error:
            print(f"RPC admission failed: {error}")
            return 1

        return 0

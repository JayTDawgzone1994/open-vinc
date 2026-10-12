"""Copy this file to data/module/pythonBusExample/pythonBusExample.py."""
import vinc


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop):
    def on_start(self, args):
        topic = f"{self.get_module_name()}/samples"
        self.publisher = self.context.advertise(vinc.messages.test, topic)
        self.subscriber = self.context.subscribe(vinc.messages.test, topic, self.received)
        self.context.param.register_param("gain", vinc.param.Type.Double, 1.0)
        self.context.param.on_change(self.parameters_changed)
        self.context.log.info(f"ready on {topic}")

    def parameters_changed(self, values):
        entry = values.get(f"{self.get_module_name()}.gain")
        if entry is not None:
            self.context.log.info(f"gain is now {entry.value}")

    def received(self, message):
        self.context.log.info(
            f"from {message.publisher_name()} publisher_id={message.publisher_instance()}: "
            f"{list(message.data().y)}"
        )

    def module_interface(self, args):
        if not args or args[0] not in ("send", "loan"):
            print("Usage: send <integers...> | loan <integers...>")
            return 1
        try:
            values = [int(value) for value in args[1:]]
            if args[0] == "send":
                message = vinc.make_message(vinc.messages.test)
                message.data().y.assign(values)
                self.publisher.push(message)
            else:
                message = self.publisher.get_next()
                message.data().y.assign(values)
                self.publisher.commit(message)
            return 0
        except (ValueError, OverflowError, vinc.VincError) as error:
            # A failed loan remains outstanding. Restart this instance before
            # borrowing again; the SDK deliberately does not auto-commit it.
            print(error)
            return 1

    def on_stop(self):
        self.subscriber.close()
        self.publisher.close()

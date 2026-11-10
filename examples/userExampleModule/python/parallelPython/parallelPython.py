"""Two independent Topic lanes with explicitly synchronized application state."""
import threading
import vinc

TOPIC_WORKERS = 2


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop,
             pump_options=vinc.PumpOptions(topic_workers=TOPIC_WORKERS)):
    def on_start(self, args):
        self.lock = threading.Lock()
        self.gain = 1
        self.received = 0
        self.latest_result = 0
        self.latest = {}
        self.context.param.register_param("gain", vinc.param.Type.Int32, 1)
        self.context.param.on_change(self.params_changed)
        self.publishers = []
        self.subscribers = []
        for topic in ("parallel/left", "parallel/right"):
            self.publishers.append(self.context.advertise(vinc.messages.int3, topic))
            self.subscribers.append(self.context.subscribe(vinc.messages.int3, topic, self.receive))

    def params_changed(self, values):
        entry = values.get(f"{self.get_module_name()}.gain")
        if entry is not None:
            with self.lock:
                self.gain = entry.value

    def receive(self, message):
        with self.lock:
            gain = self.gain
        # Perform bounded work outside the application lock. Pure Python CPU work
        # still shares the GIL; I/O or native code may release it.
        result = message.data().x * gain
        with self.lock:
            self.latest_result = result
            self.received += 1
            # Retain the independently owned delivery; no message.copy() needed.
            self.latest[message.topic_name()] = message

    def module_interface(self, args):
        if args == ["send"]:
            message = vinc.make_message(vinc.messages.int3)
            message.data().x = 42
            try:
                # Publisher access stays on the serial control dispatcher.
                for publisher in self.publishers:
                    publisher.push(message)
            except vinc.VincError as error:
                print(error)
                return 1
        else:
            with self.lock:
                count = self.received
                result = self.latest_result
            print(f"{self.get_module_name()} received={count} scaled={result}")
        return 0

    def on_stop(self):
        # Runtime drained all framework callbacks before this hook.
        self.latest.clear()

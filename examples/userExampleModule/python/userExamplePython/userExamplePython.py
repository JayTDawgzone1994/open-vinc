"""Same Shell commands and message layout as the C++ userExampleModule."""
import copy
import vinc

MESSAGE = vinc.messages.UserExampleFrame
MESSAGE_LIMIT_BYTES = 64 * 1024
RING_SLOTS = 8
INITIAL_GAIN = 1.0
RESERVED_SAMPLES = 8


def fill(message, sequence):
    fields = message.data()
    fields.sequence = sequence
    fields.note.assign("SDK walkthrough")
    fields.samples.assign([10, 20, 30])
    fields.flags.assign([True, False])
    fields.items.resize(1)
    # This is a borrowed view. Python indexes are checked, including negative indexes.
    item = fields.items[0]
    item.id = 7
    item.score = 0.95
    item.label.assign("camera")
    fields.groups.resize(1)
    fields.groups[0].assign(fields.items)


def describe(message):
    print(f"MESSAGE sequence={message.data().sequence} topic={message.topic_name()} "
          f"publisher={message.publisher_name()} publisher_id={message.publisher_instance()} "
          f"count={message.get_topic_count()} timestamp_ns={message.get_message_timestamp()} "
          f"samples={len(message.data().samples)} note={message.data().note.decode()}")
    if len(message.data().groups) and len(message.data().groups[0]):
        item = message.data().groups[0][0]
        print(f"NESTED id={item.id} label={item.label.decode()}")


def demonstrate_storage():
    message = vinc.make_message(MESSAGE)
    fill(message, 42)
    samples = message.data().samples
    samples.reserve(RESERVED_SAMPLES)
    samples[0] = 11
    samples.set(1, 22)
    samples.push_back(40)
    samples.push_back(50)  # Python has no separate emplace_back API.
    samples.pop_back()
    samples.shrink_to_fit()
    print(f"ARRAY size={len(samples)} capacity={samples.capacity} "
          f"empty={len(samples) == 0} values={list(samples)}")
    independent_array = samples.copy()
    independent_array.clear()
    independent_array.assign(samples)

    item = message.data().items[0]
    alias = item
    alias.score = 0.5
    independent_item = copy.copy(item)
    independent_item.id = 8
    message.data().items.set(0, independent_item)
    checked_copy = message.data().items.at(0)
    checked_copy.id = 9  # Does not modify the original item.
    try:
        samples.at(len(samples))
    except IndexError as error:
        print(f"EXPECTED checked access: {error}")

    message.data().note.assign(b"a\0b")
    assert message.data().note.to_bytes() == b"a\0b"
    message.compact()
    usage = message.usage()
    image = message.serialize()
    decoded = vinc.make_message(MESSAGE)
    decoded.deserialize(image)
    cloned = decoded.copy()
    cloned.assign(message)
    print(f"STORAGE_OK type=UserExampleFrame hash={MESSAGE.schema_hash} "
          f"protocol={MESSAGE.constants['protocol']} compact_bytes={usage.compact_bytes} "
          f"allocated_bytes={usage.allocated_bytes} limit_bytes={usage.limit_bytes}")


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop):
    def on_name(self, args):
        # The standard Python hook consumes --name and returns the requested name.
        # Override this method to implement an application's own naming convention.
        return super().on_name(args)

    def on_start(self, args):
        self.publisher = None
        self.subscriber = None
        self.latest = None
        self.retained = None
        self.gain = INITIAL_GAIN
        self.topic = "userExample/frames"
        if args:
            if len(args) != 2 or args[0] != "--topic":
                raise ValueError("Usage: --name <instance> [--topic <topic>]")
            self.topic = args[1]
        self.register_parameters()
        self.open_publisher()
        self.open_subscribers()
        print(f"READY {self.get_module_name()} mid={self.get_mid()} topic={self.topic}")

    def register_parameters(self):
        self.context.param.register_param("gain", vinc.param.Type.Double, INITIAL_GAIN,
                                          vinc.param.Permission.Open)
        self.context.param.register_param("label", vinc.param.Type.String, "example")
        self.context.param.register_param("locked_gain", vinc.param.Type.Double, INITIAL_GAIN)
        self.context.param.register_param("peer.gain", vinc.param.Type.Double)
        self.context.param.on_change(self.parameters_changed)

    def parameters_changed(self, values):
        # Python receives an independent read-only snapshot; C++ receives a borrowed map.
        entry = values.get(f"{self.get_module_name()}.gain")
        if entry is not None:
            self.gain = entry.value
            print(f"PARAM {self.get_module_name()} gain={self.gain} version={entry.version}")
        if "peer.gain" not in values:
            print("WATCH peer.gain missing")

    def open_publisher(self):
        options = vinc.message_bus.PublisherOptions()
        options.slots = RING_SLOTS
        options.max_message_bytes = MESSAGE_LIMIT_BYTES
        self.publisher = self.context.advertise(MESSAGE, self.topic, options)

    def open_subscribers(self):
        options = vinc.message_bus.SubscriberOptions()
        options.start = vinc.message_bus.StartPosition.OldestRetained
        options.max_message_bytes = MESSAGE_LIMIT_BYTES
        self.subscriber = self.context.subscribe(MESSAGE, self.topic, self.received, options)
        options.start = vinc.message_bus.StartPosition.NewOnly
        self.latest = self.context.subscribe(MESSAGE, self.topic, options=options)

    def received(self, message):
        print(f"CALLBACK {self.get_module_name()}", end=" ")
        describe(message)
        # The local message can be retained or changed after this callback returns.
        self.retained = message

    def publish(self, sequence, use_loan):
        if self.publisher is None:
            raise ValueError("Publisher closed; run reopen first")
        if not use_loan:
            local = vinc.make_message(MESSAGE)
            fill(local, sequence)
            self.publisher.push(local)
            return
        loan = self.publisher.get_next()
        try:
            fill(loan, sequence)
            self.publisher.commit(loan)
        except (vinc.VincError, ValueError, TypeError, OverflowError):
            # Explicit application recovery: a discarded loan alone is not cancelled.
            self.publisher.close()
            self.publisher = None
            raise
        # Commit revoked the loan and all its borrowed views. Do not access them here.

    def module_interface(self, args):
        try:
            self.execute(args)
            return 0
        except vinc.VincError as error:
            print(vinc.error_to_string(error.error))
            print(f"domain={error.error.domain} code={error.error.code} "
                  f"operation={error.error.operation} causes={len(error.error.causes)} "
                  f"truncated={error.error.truncated}")
            return 1
        except (ValueError, TypeError, OverflowError) as error:
            print(error)
            return 1
        # Unhandled exceptions in user hooks/callbacks still fail this Module.

    def module_autoComplete(self, args):
        return ["help", "status", "push", "loan", "last", "topics", "storage",
                "capacity", "params", "set", "logs", "reopen"]

    def execute(self, args):
        if not args or args[0] == "help":
            print("help | status | push <sequence> | loan <sequence> | last | topics | storage")
            print("capacity | params | set <full-or-own-name> <double> | logs | reopen")
            return
        command = args[0]
        if command in ("push", "loan") and len(args) == 2:
            self.publish(int(args[1]), command == "loan")
        elif command == "storage":
            demonstrate_storage()
        elif command == "last":
            describe(self.latest.get_last())
        elif command == "status":
            print(f"STATUS {self.get_module_name()} mid={self.get_mid()} "
                  f"living={self.context.living()} gain={self.gain}")
            if self.publisher is not None:
                stats = self.publisher.stats()
                print(f"PUB published={stats.published} slots={stats.effective_slots} "
                      f"shared_bytes={stats.shared_bytes} collisions={stats.collisions} "
                      f"total_wait_ns={stats.total_wait_ns} longest_wait_ns={stats.longest_wait_ns} "
                      f"current_wait_ns={stats.current_wait_ns}")
            if self.subscriber is not None:
                stats = self.subscriber.stats()
                print(f"SUB lost={stats.lost_messages} overwrite_events={stats.overwrite_events} "
                      f"oversized={stats.oversized_messages}")
                for source in stats.sources:
                    print(f"SOURCE publisher_id={source.publisher} lost={source.lost_messages}")
        elif command == "topics":
            after = ""
            while True:
                page = self.context.list_topics(after)
                for topic in page.topics:
                    print(f"TOPIC {topic.name} schema={topic.schema} publishers={topic.publishers}")
                after = page.next
                if not after:
                    break
        elif command == "params":
            gain = self.context.param.update("gain", vinc.param.Type.Double)
            print(f"CACHE gain={gain}")
            for watch in self.context.param.watches():
                print(f"WATCH {watch.name} present={watch.present} compatible={watch.compatible}")
        elif command == "set" and len(args) == 3:
            self.context.param.set(args[1], vinc.param.Type.Double, float(args[2]))
        elif command == "logs":
            self.demonstrate_logs()
        elif command == "capacity":
            oversized = vinc.make_message(MESSAGE)
            oversized.data().note.resize(MESSAGE_LIMIT_BYTES)
            try:
                self.publisher.push(oversized)
            except vinc.VincError as error:
                print(f"EXPECTED capacity: {error}")
            else:
                raise RuntimeError("oversized message unexpectedly published")
        elif command == "reopen":
            self.close_bus()
            self.open_publisher()
            self.open_subscribers()
        else:
            raise ValueError("Unknown command or arguments; run help")

    def demonstrate_logs(self):
        # This application treats rejected logs as nonfatal and reports them via raw output.
        for write in (self.context.log.error, self.context.log.warn,
                      self.context.log.info, self.context.log.debug):
            try:
                write(f"Level demonstration from {self.get_module_name()}")
            except vinc.VincError as error:
                print(error)
        try:
            self.context.log.write(vinc.log.Level.Info, "Already formatted text")
            if self.context.log.enabled(vinc.log.Level.Debug):
                self.context.log.debug(f"Cached gain = {self.gain}")
        except vinc.VincError as error:
            print(error)
        print("RAW print from userExamplePython")

    def close_bus(self):
        for handle in (self.subscriber, self.latest, self.publisher):
            if handle is not None:
                handle.close()
        self.subscriber = None
        self.latest = None
        self.publisher = None

    def on_stop(self):
        self.close_bus()
        self.retained = None
        print(f"STOPPED {self.get_module_name()}")

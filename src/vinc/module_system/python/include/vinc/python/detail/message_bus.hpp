#pragma once
#include <vinc/python/detail/message_view.hpp>
#include <vinc/python/detail/callback.hpp>
#include <optional>

namespace vinc::python {
// A checked clone creates the payload through Result before copying delivery
// metadata. The derived facade can use BaseMessage's protected copy contract
// without exposing a metadata setter to Python or adding a public C++ setter.
template<class T> class CopiedMessage final : public T {
public:
    CopiedMessage(storage::Binding binding, const T& source)
        : T(storage::RecordAccess::bind<T>(std::move(binding))) {
        BaseMessage::operator=(source);
    }
};

template<class T> struct MessageValue : MessageNode {
    T* value = nullptr;

    static MessageValue adopt(std::unique_ptr<T> message) {
        auto owner = std::make_shared<PayloadOwner>();
        auto pointer = message.get();
        auto binding = storage::RecordAccess::binding(*pointer);
        owner->backing = std::shared_ptr<T>(std::move(message));
        return {{std::move(binding), std::move(owner)}, pointer};
    }

    static MessageValue create() {
        auto binding = unwrap(storage::create_record(T::schema()));
        return adopt(std::make_unique<T>(storage::RecordAccess::bind<T>(std::move(binding))));
    }

    View<T> data() const {
        check();
        return {static_cast<const MessageNode&>(*this)};
    }
};

// Bus handles cannot be closed/moved while a released-GIL operation is using
// them. The native Context still owns process-level transport and supervision.
struct ResourceUse {
    bool& busy;
    explicit ResourceUse(bool& value) : busy(value) {
        if (busy) {
            raise_error(Error{message_bus::ErrorCode::OperationInProgress, message_bus::ErrorOperation::Submit});
        }

        busy = true;
    }
    ~ResourceUse() {
        busy = false;
    }
};

template<class T> struct LoanState final : PythonResource {
    std::optional<message_bus::WriteLoan<T>> value;
    std::weak_ptr<PayloadOwner> owner;

    LoanState() = default;
    void clear() override {
        if (auto retained = owner.lock()) {
            retained->valid = false;
        }
        value.reset();
    }
};

template<class T> struct LoanValue : MessageNode {
    std::shared_ptr<LoanState<T>> loan;
    View<T> data() const {
        check();
        return {static_cast<const MessageNode&>(*this)};
    }
};

template<class T> std::pair<std::shared_ptr<LoanState<T>>, std::shared_ptr<PayloadOwner>>
prepare_loan(const std::shared_ptr<ContextState>& state) {
    // Allocate/register the Python lifetime bookkeeping before reserving a Bus
    // slot. Failure here cannot leave an outstanding native loan behind.
    auto loan = std::make_shared<LoanState<T>>();
    auto owner = std::make_shared<PayloadOwner>();
    owner->backing = loan;
    loan->owner = owner;
    state->track(loan);
    return {std::move(loan), std::move(owner)};
}

template<class T> struct PythonPublisher final : PythonResource {
    std::shared_ptr<ContextState> state;
    message_bus::Publisher<T> native;
    std::weak_ptr<LoanState<T>> outstanding;
    bool busy = false;

    PythonPublisher(std::shared_ptr<ContextState> context, message_bus::Publisher<T> publisher)
        : state(std::move(context)), native(std::move(publisher)) {
    }

    ~PythonPublisher() override {
        clear();
    }

    void clear() override {
        if (auto loan = outstanding.lock()) {
            loan->clear();
        }
        native.close();
    }
    void close() {
        ContextCall call(state);
        ResourceUse use(busy);
        if (auto loan = outstanding.lock()) {
            loan->clear();
        }
        py::gil_scoped_release release;
        native.close();
    }
    void push(const MessageValue<T>& message) {
        ContextCall call(state);
        ResourceUse use(busy);
        PayloadUse payload(message);
        Result<void> result;
        {
            py::gil_scoped_release release;
            result = native.push(*message.value);
        }
        unwrap(std::move(result));
    }
    LoanValue<T> get_next() {
        ContextCall call(state);
        ResourceUse use(busy);
        auto [loan, owner] = prepare_loan<T>(state);
        auto result = [&] {
            py::gil_scoped_release release;
            return native.get_next();
        }();
        loan->value.emplace(unwrap(std::move(result)));
        auto binding = storage::RecordAccess::binding(**loan->value);
        outstanding = loan;
        return {{std::move(binding), std::move(owner)}, std::move(loan)};
    }
    void commit(const LoanValue<T>& message) {
        ContextCall call(state);
        ResourceUse use(busy);
        PayloadUse payload(message);
        if (!message.loan->value) {
            expired_message();
        }

        Result<void> result;
        {
            py::gil_scoped_release release;
            result = native.commit(*message.loan->value);
        }
        if (result) {
            message.loan->clear();
        }
        unwrap(std::move(result));
    }
    message_bus::PublisherStats stats() {
        ContextCall call(state);
        ResourceUse use(busy);
        py::gil_scoped_release release;
        return native.stats();
    }
};

template<class T> struct PythonSubscriber final : PythonResource {
    std::shared_ptr<ContextState> state;
    message_bus::Subscriber<T> native;
    std::shared_ptr<PythonCallback> callback;
    bool busy = false;

    PythonSubscriber(std::shared_ptr<ContextState> context, message_bus::Subscriber<T> subscriber,
                     std::shared_ptr<PythonCallback> function)
        : state(std::move(context)), native(std::move(subscriber)), callback(std::move(function)) {
    }
    void clear() override {
        native.close();
        if (callback) {
            callback->clear();
        }
    }
    void close() {
        ContextCall call(state);
        ResourceUse use(busy);
        {
            py::gil_scoped_release release;
            native.close();
        }
        if (callback) {
            callback->clear();
        }
    }
    MessageValue<T> get_last() {
        ContextCall call(state);
        ResourceUse use(busy);
        auto result = [&] {
            py::gil_scoped_release release;
            return native.get_last();
        }();
        // Native reception already made the framework's required local copy.
        // Transfer its ownership to Python without another image copy.
        return MessageValue<T>::adopt(unwrap(std::move(result)));
    }
    message_bus::SubscriberStats stats() {
        ContextCall call(state);
        ResourceUse use(busy);
        py::gil_scoped_release release;
        return native.stats();
    }
};

template<class T> void bind_message(py::module_& module, py::module_& views) {
    bind_view<T>(views);
    auto name = std::string(T::message_name);
    using Value = MessageValue<T>;
    auto type = py::class_<Value>(module, name.c_str());
    py::dict constants;
    T::ribosome_visit_constants([&](const char* name, const auto& value) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, std::string_view>) {
            // Constants obey the same byte-preserving string policy as fields.
            constants[py::str(name)] = py::bytes(value.data(), value.size());
        } else {
            constants[py::str(name)] = py::cast(value);
        }
    });
    type.attr("constants") = py::module_::import("types").attr("MappingProxyType")(constants);
    type.def(py::init(&Value::create));
    type.def("data", &Value::data);
    type.def("copy", [](const Value& value) {
        auto node = detached(value);
        return Value::adopt(std::make_unique<CopiedMessage<T>>(std::move(node.binding), *value.value));
    });
    type.def("assign", [](const Value& value, const Value& other) {
        value.check();
        other.check();
        unwrap(storage::replace(value.binding, other.binding));
    });
    type.def("usage", [](const Value& value) {
        value.check();
        return unwrap(storage::usage(value.binding.state));
    });
    type.def("compact", [](const Value& value) {
        value.check();
        unwrap(storage::compact(value.binding.state));
    });
    type.def("serialize", [](const Value& value) {
        value.check();
        std::vector<std::uint8_t> bytes;
        unwrap(value.value->serialize(bytes));
        // Python's bytes constructor copies byte-addressable uint8_t storage.
        return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    });
    type.def("_rpc_snapshot", [](const Value& value, std::uint64_t maximum) {
        value.check();
        auto image = unwrap(value.value->snapshot(maximum));
        auto bytes = image.bytes();
        // Python copies a byte image; no generated C++ object layout crosses the ABI.
        return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    });
    type.def("deserialize", [](const Value& value, py::bytes image) {
        value.check();
        auto bytes = py::cast<std::string>(image);
        std::vector<std::uint8_t> buffer(bytes.begin(), bytes.end());
        unwrap(value.value->deserialize(buffer));
    });
    type.def("topic_name", [](const Value& value) {
        value.check();
        return value.value->topic_name();
    });
    type.def("publisher_name", [](const Value& value) {
        value.check();
        return value.value->publisher_name();
    });
    type.def("publisher_instance", [](const Value& value) {
        value.check();
        return value.value->publisher_instance();
    });
    type.def("get_message_timestamp", [](const Value& value) {
        value.check();
        return value.value->get_message_timestamp();
    });
    type.def("get_message_monotonic_timestamp", [](const Value& value) {
        value.check();
        return value.value->get_message_monotonic_timestamp();
    });
    type.def("get_topic_count", [](const Value& value) {
        value.check();
        return value.value->get_topic_count();
    });
    type.def_property_readonly_static("schema_hash", [](py::object) {
        return T::schema().hash;
    });
    using Publisher = PythonPublisher<T>;
    using Subscriber = PythonSubscriber<T>;
    py::class_<LoanValue<T>>(module, ("_Loan_" + name).c_str())
        .def("data", &LoanValue<T>::data)
        .def("usage", [](const LoanValue<T>& value) {
            value.check();
            return unwrap(storage::usage(value.binding.state));
        });
    py::class_<Publisher, std::shared_ptr<Publisher>>(module, ("_Publisher_" + name).c_str())
        .def("push", &Publisher::push)
        .def("get_next", &Publisher::get_next)
        .def("commit", &Publisher::commit)
        .def("close", &Publisher::close)
        .def("stats", &Publisher::stats);
    py::class_<Subscriber, std::shared_ptr<Subscriber>>(module, ("_Subscriber_" + name).c_str())
        .def("get_last", &Subscriber::get_last)
        .def("close", &Subscriber::close)
        .def("stats", &Subscriber::stats);
    type.def_static("_advertise", [](std::shared_ptr<ContextState> state, std::string topic,
                                    message_bus::PublisherOptions options) {
        ContextCall call(state);
        auto result = [&] {
            py::gil_scoped_release release;
            return call.context().advertise<T>(std::move(topic), options);
        }();
        auto publisher = std::make_shared<Publisher>(state, unwrap(std::move(result)));
        state->track(publisher);
        return publisher;
    });
    type.def_static("_subscribe", [](std::shared_ptr<ContextState> state, std::string topic,
                                    py::object function, message_bus::SubscriberOptions options) {
        ContextCall call(state);
        std::shared_ptr<PythonCallback> retained;
        if (!function.is_none()) {
            retained = callback(std::move(function), state, ErrorOperation::SubscriptionCallback);
        }
        auto result = [&] {
            py::gil_scoped_release release;
            if (!retained) {
                return call.context().subscribe<T>(std::move(topic), options);
            }

            return call.context().subscribe<T>(std::move(topic), [retained](std::unique_ptr<T> value) {
                retained->invoke([&] {
                    return py::cast(Value::adopt(std::move(value)));
                });
            }, options);
        }();
        auto subscriber = std::make_shared<Subscriber>(state, unwrap(std::move(result)), retained);
        state->track(subscriber);
        return subscriber;
    });
}
}

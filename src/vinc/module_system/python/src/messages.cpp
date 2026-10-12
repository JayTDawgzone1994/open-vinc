#include <vinc/python/detail/message_bus.hpp>
#include <vinc_messages/message_types.hpp>

namespace vinc::python {
#ifdef VINC_PYTHON_TEST_MESSAGES
void bind_fixture_messages(py::module_& module, py::module_& views);
#endif
void bind_messages(py::module_& module) {
    auto messages = module.def_submodule("messages");
    auto views = module.def_submodule("_message_views");
    auto bus = module.def_submodule("message_bus");
    py::class_<message::Usage>(bus, "Usage")
        .def_readonly("compact_bytes", &message::Usage::compact_bytes)
        .def_readonly("allocated_bytes", &message::Usage::allocated_bytes)
        .def_readonly("limit_bytes", &message::Usage::limit_bytes);
    py::class_<message_bus::PublisherOptions>(bus, "PublisherOptions")
        .def(py::init<>())
        .def_readwrite("slots", &message_bus::PublisherOptions::slots)
        .def_readwrite("max_message_bytes", &message_bus::PublisherOptions::max_message_bytes);
    py::enum_<message_bus::StartPosition>(bus, "StartPosition")
        .value("OldestRetained", message_bus::StartPosition::OldestRetained)
        .value("NewOnly", message_bus::StartPosition::NewOnly);
    py::class_<message_bus::SubscriberOptions>(bus, "SubscriberOptions")
        .def(py::init<>())
        .def_readwrite("start", &message_bus::SubscriberOptions::start)
        .def_readwrite("max_message_bytes", &message_bus::SubscriberOptions::max_message_bytes);
    py::class_<message_bus::PublisherStats>(bus, "PublisherStats")
        .def_readonly("requested_slots", &message_bus::PublisherStats::requested_slots)
        .def_readonly("effective_slots", &message_bus::PublisherStats::effective_slots)
        .def_readonly("max_message_bytes", &message_bus::PublisherStats::max_message_bytes)
        .def_readonly("shared_bytes", &message_bus::PublisherStats::shared_bytes)
        .def_readonly("published", &message_bus::PublisherStats::published)
        .def_readonly("collisions", &message_bus::PublisherStats::collisions)
        .def_readonly("total_wait_ns", &message_bus::PublisherStats::total_wait_ns)
        .def_readonly("longest_wait_ns", &message_bus::PublisherStats::longest_wait_ns)
        .def_readonly("current_wait_ns", &message_bus::PublisherStats::current_wait_ns);
    py::class_<message_bus::PublisherLoss>(bus, "PublisherLoss")
        .def_readonly("publisher", &message_bus::PublisherLoss::publisher)
        .def_readonly("overwrite_events", &message_bus::PublisherLoss::overwrite_events)
        .def_readonly("lost_messages", &message_bus::PublisherLoss::lost_messages);
    py::class_<message_bus::SubscriberStats>(bus, "SubscriberStats")
        .def_readonly("oversized_messages", &message_bus::SubscriberStats::oversized_messages)
        .def_readonly("overwrite_events", &message_bus::SubscriberStats::overwrite_events)
        .def_readonly("lost_messages", &message_bus::SubscriberStats::lost_messages)
        .def_readonly("sources", &message_bus::SubscriberStats::sources);
    py::class_<message_bus::TopicInfo>(bus, "TopicInfo")
        .def_readonly("name", &message_bus::TopicInfo::name)
        .def_readonly("schema", &message_bus::TopicInfo::schema)
        .def_readonly("publishers", &message_bus::TopicInfo::publishers);
    py::class_<message_bus::TopicPage>(bus, "TopicPage")
        .def_readonly("topics", &message_bus::TopicPage::topics)
        .def_readonly("next", &message_bus::TopicPage::next);
    message::visit_types([&]<class T> {
        bind_message<T>(messages, views);
    });
#ifdef VINC_PYTHON_TEST_MESSAGES
    bind_fixture_messages(module, views);
#endif
    auto context = py::reinterpret_borrow<py::class_<ContextState, std::shared_ptr<ContextState>>>(module.attr("Context"));
    context.def("advertise", [](std::shared_ptr<ContextState> state, py::object type,
                                std::string topic, message_bus::PublisherOptions options) {
        return type.attr("_advertise")(state, std::move(topic), options);
    }, py::arg("type"), py::arg("topic"), py::arg("options") = message_bus::PublisherOptions{});
    context.def("subscribe", [](std::shared_ptr<ContextState> state, py::object type,
                                std::string topic, py::object function, message_bus::SubscriberOptions options) {
        return type.attr("_subscribe")(state, std::move(topic), std::move(function), options);
    }, py::arg("type"), py::arg("topic"), py::arg("callback") = py::none(),
       py::arg("options") = message_bus::SubscriberOptions{});
    context.def("list_topics", [](std::shared_ptr<ContextState> state, std::string after) {
        ContextCall call(state);
        auto result = [&] {
            py::gil_scoped_release release;
            return call.context().list_topics(std::move(after));
        }();
        return unwrap(std::move(result));
    }, py::arg("after") = "");
}
}

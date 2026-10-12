#include <vinc/python/detail/callback.hpp>
#include <vinc/python/detail/conversion.hpp>

namespace vinc::python {
void clear_resources(const std::shared_ptr<ContextState>& state) {
    // Called with the GIL after admitted calls drain. Swap first: clearing a
    // callable may execute Python destructors that release other resources.
    auto resources = std::move(state->resources);
    for (auto& weak : resources) {
        if (auto resource = weak.lock()) {
            resource->clear();
        }
    }
}

namespace {
template<class Function> auto visit_type(param::Type type, Function&& function) {
    switch (type) {
    case param::Type::Bool:
        return function.template operator()<bool>();
    case param::Type::Int8:
        return function.template operator()<std::int8_t>();
    case param::Type::UInt8:
        return function.template operator()<std::uint8_t>();
    case param::Type::Int16:
        return function.template operator()<std::int16_t>();
    case param::Type::UInt16:
        return function.template operator()<std::uint16_t>();
    case param::Type::Int32:
        return function.template operator()<std::int32_t>();
    case param::Type::UInt32:
        return function.template operator()<std::uint32_t>();
    case param::Type::Int64:
        return function.template operator()<std::int64_t>();
    case param::Type::UInt64:
        return function.template operator()<std::uint64_t>();
    case param::Type::Float:
        return function.template operator()<float>();
    case param::Type::Double:
        return function.template operator()<double>();
    case param::Type::String:
        return function.template operator()<std::string>();
    }
    throw py::value_error("unknown parameter type");
}

template<class T> T parameter_value(py::handle value) {
    if constexpr (std::is_same_v<T, std::string>) {
        if (!PyUnicode_Check(value.ptr())) {
            throw py::type_error("string parameters require str");
        }

        return py::cast<std::string>(value);
    } else {
        return scalar_value<T>(value);
    }
}

struct Params {
    std::shared_ptr<ContextState> state;

    void register_param(std::string name, param::Type type, py::object defaults, param::Permission permission) {
        ContextCall call(state);
        visit_type(type, [&]<class T> {
            Result<void> result;
            if (defaults.is_none()) {
                py::gil_scoped_release release;
                result = call.context().param.register_param<T>(std::move(name));
            } else {
                auto value = parameter_value<T>(defaults);
                py::gil_scoped_release release;
                result = call.context().param.register_param<T>(std::move(name), std::move(value), permission);
            }
            unwrap(std::move(result));
        });
    }

    py::object update(std::string name, param::Type type) {
        ContextCall call(state);
        return visit_type(type, [&]<class T> {
            T value{};
            Result<void> result;
            {
                py::gil_scoped_release release;
                result = call.context().param.update(name, value);
            }
            unwrap(std::move(result));
            return py::cast(value);
        });
    }

    void set(std::string name, param::Type type, py::object value) {
        ContextCall call(state);
        visit_type(type, [&]<class T> {
            auto converted = parameter_value<T>(value);
            Result<void> result;
            {
                py::gil_scoped_release release;
                result = call.context().param.set<T>(std::move(name), std::move(converted));
            }
            unwrap(std::move(result));
        });
    }

    void on_change(py::object function) {
        ContextCall call(state);
        std::function<void(const param::ParamMap&)> callback_function;
        if (!function.is_none()) {
            auto retained = callback(std::move(function), state, ErrorOperation::ParamCallback);
            callback_function = [retained](const param::ParamMap& values) {
                retained->invoke([&] {
                    py::dict snapshot;
                    for (const auto& [name, entry] : values) {
                        snapshot[py::str(name)] = py::cast(entry, py::return_value_policy::copy);
                    }
                    return py::module_::import("types").attr("MappingProxyType")(snapshot);
                });
            };
        }
        unwrap(call.context().param.on_change(std::move(callback_function)));
    }
};

struct Logger {
    std::shared_ptr<ContextState> state;
    bool enabled(log::Level level) {
        ContextCall call(state);
        return call.context().log.enabled(level);
    }
    void write(log::Level level, std::string text) {
        ContextCall call(state);
        Result<void> result;
        {
            py::gil_scoped_release release;
            result = call.context().log.write(level, text);
        }
        unwrap(std::move(result));
    }
};
}

void bind_services(py::module_& module) {
    auto params = module.def_submodule("param");
    py::enum_<param::Type>(params, "Type")
        .value("Bool", param::Type::Bool)
        .value("Int8", param::Type::Int8).value("UInt8", param::Type::UInt8)
        .value("Int16", param::Type::Int16).value("UInt16", param::Type::UInt16)
        .value("Int32", param::Type::Int32).value("UInt32", param::Type::UInt32)
        .value("Int64", param::Type::Int64).value("UInt64", param::Type::UInt64)
        .value("Float", param::Type::Float).value("Double", param::Type::Double)
        .value("String", param::Type::String);
    py::enum_<param::Permission>(params, "Permission")
        .value("FrameworkOnly", param::Permission::FrameworkOnly)
        .value("Open", param::Permission::Open);
    py::class_<param::Entry>(params, "Entry")
        .def_property_readonly("type", [](const param::Entry& entry) {
            return param::type_of(entry.value);
        })
        .def_property_readonly("value", [](const param::Entry& entry) {
            return std::visit([](const auto& value) {
                return py::cast(value);
            }, entry.value);
        })
        .def_property_readonly("default_value", [](const param::Entry& entry) {
            return std::visit([](const auto& value) {
                return py::cast(value);
            }, entry.default_value);
        })
        .def_readonly("permission", &param::Entry::permission)
        .def_readonly("version", &param::Entry::version)
        .def_readonly("timestamp", &param::Entry::timestamp);
    py::class_<param::WatchStatus>(params, "WatchStatus")
        .def_readonly("name", &param::WatchStatus::name)
        .def_readonly("expected", &param::WatchStatus::expected)
        .def_readonly("present", &param::WatchStatus::present)
        .def_readonly("compatible", &param::WatchStatus::compatible);
    py::class_<Params>(module, "Params")
        .def("register_param", &Params::register_param, py::arg("name"), py::arg("type"),
             py::arg("default") = py::none(), py::arg("permission") = param::Permission::FrameworkOnly)
        .def("update", &Params::update)
        .def("set", &Params::set)
        .def("on_change", &Params::on_change)
        .def("watches", [](Params& self) {
            ContextCall call(self.state);
            return call.context().param.watches();
        });
    auto logs = module.def_submodule("log");
    py::enum_<log::Level>(logs, "Level")
        .value("Error", log::Level::Error).value("Warn", log::Level::Warn)
        .value("Info", log::Level::Info).value("Debug", log::Level::Debug);
    py::class_<Logger>(module, "Logger")
        .def("enabled", &Logger::enabled)
        .def("write", &Logger::write)
        .def("info", [](Logger& self, std::string text) {
            self.write(log::Level::Info, std::move(text));
        })
        .def("warn", [](Logger& self, std::string text) {
            self.write(log::Level::Warn, std::move(text));
        })
        .def("error", [](Logger& self, std::string text) {
            self.write(log::Level::Error, std::move(text));
        })
        .def("debug", [](Logger& self, std::string text) {
            self.write(log::Level::Debug, std::move(text));
        });
    // Extend the already registered Context class; these facades only carry its
    // revocable admission token, never a separately owned framework service.
    auto context = py::reinterpret_borrow<py::class_<ContextState, std::shared_ptr<ContextState>>>(module.attr("Context"));
    context.def_property_readonly("param", [](std::shared_ptr<ContextState> state) {
        return Params{state};
    });
    context.def_property_readonly("log", [](std::shared_ptr<ContextState> state) {
        return Logger{state};
    });
}
}

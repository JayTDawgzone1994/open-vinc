#include <vinc/python/detail/bridge.hpp>
#include <pybind11/stl.h>
#include <vinc/module/detail/module_runtime.hpp>
#include <vinc/module/detail/module_object.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/module/error.hpp>
#include <vinc/config.hpp>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>

namespace py = pybind11;
namespace vinc::python {
using module_system::ErrorCode;
using module_system::ErrorOperation;

Error report_exception(py::error_already_set& exception, ErrorOperation operation,
                       std::string_view name, MID mid) {
    Error error{ErrorCode::PythonException, operation};
    // Preserve VINC's structured cause when an uncaught SDK exception escapes.
    auto type = py::module_::import("vinc").attr("VincError");
    if (exception.matches(type)) {
        error = wrap_error(ErrorCode::PythonException, operation,
                           exception.value().attr("error").cast<Error>());
    }

    std::string_view text(exception.what());
    std::cerr << "Python Module " << name << " MID=" << mid << ":\n"
              << text.substr(0, config::python_traceback_bytes);
    if (text.size() > config::python_traceback_bytes) {
        std::cerr << "\n[traceback truncated]";
    }

    std::cerr << '\n' << std::flush;
    return error;
}

void require_none(const py::object& result) {
    if (!result.is_none()) {
        throw py::type_error("Lifecycle hooks return None; raise an exception on failure");
    }
}

class PythonModule final : public detail::ModuleObject {
    py::object instance_;
    bool manual_ = false;
    std::shared_ptr<ContextState> state_ = std::make_shared<ContextState>();

    template <class T, class Function>
    Result<T> invoke(ErrorOperation operation, Function&& function) {
        py::gil_scoped_acquire gil;
        try {
            if constexpr (std::is_void_v<T>) {
                function();
                return {};
            } else {
                return function();
            }
        } catch (py::error_already_set& error) {
            return std::unexpected(report_exception(error, operation, get_module_name(), get_mid()));
        } catch (const py::builtin_exception& error) {
            error.set_error();
            py::error_already_set converted;
            return std::unexpected(report_exception(converted, operation, get_module_name(), get_mid()));
        }
    }

public:
    explicit PythonModule(std::string name) : ModuleObject(std::move(name)) {
        state_->module = this;
        state_->context = &context;
    }

    ~PythonModule() override {
        // Runtime calls destruction without the GIL. Let outstanding Python
        // pump calls reacquire it and unwind before invalidating their pointer.
        state_->revoke();
        py::gil_scoped_acquire gil;
        clear_resources(state_);
        instance_ = py::object{};
    }

    Result<void> initialize(const py::object& type) {
        return invoke<void>(ErrorOperation::ConstructModule, [&] {
            auto sdk = py::module_::import("vinc");
            if (!py::isinstance<py::type>(type) ||
                !py::module_::import("builtins").attr("issubclass")(type, sdk.attr("BaseModule")).cast<bool>()) {
                throw py::type_error("Script must export class Module(vinc.BaseModule, mode=...)");
            }

            manual_ = type.attr("_vinc_mode").is(sdk.attr("RunMode").attr("Manual"));
            instance_ = type.attr("__new__")(type);
            instance_.attr("_vinc_context") = py::cast(state_);
            require_none(instance_.attr("__init__")());
        });
    }

    Result<std::string> on_name(std::vector<std::string>& args) override {
        return invoke<std::string>(ErrorOperation::NameHook, [&] {
            // Existing central callers may include the ModuleType at argv[0].
            // The source strips that token before any Python hook is entered.
            py::list values = py::cast(args);
            auto name = instance_.attr("on_name")(values).cast<std::string>();
            args = values.cast<std::vector<std::string>>();
            return name;
        });
    }

    Result<void> on_start(const std::vector<std::string>& args) override {
        return invoke<void>(ErrorOperation::StartHook, [&] {
            require_none(instance_.attr("on_start")(args));
        });
    }

    Result<void> main() override {
        if (!manual_) {
            return context.pumpLoop();
        }

        return invoke<void>(ErrorOperation::MainHook, [&] {
            require_none(instance_.attr("main")());
        });
    }

    Result<void> on_stop() override {
        return invoke<void>(ErrorOperation::StopHook, [&] {
            require_none(instance_.attr("on_stop")());
        });
    }

    Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        return invoke<std::int32_t>(ErrorOperation::CommandHook, [&] {
            if (!args.empty() && args.front() == get_module_name()) {
                args.erase(args.begin());
            }

            auto result = instance_.attr("module_interface")(args);
            if (!PyLong_Check(result.ptr()) || PyBool_Check(result.ptr())) {
                throw py::type_error("module_interface must return an int32 command status");
            }

            return result.cast<std::int32_t>();
        });
    }

    Result<std::vector<std::string>> module_autoComplete(std::vector<std::string> args) override {
        return invoke<std::vector<std::string>>(ErrorOperation::CompleteHook, [&] {
            if (!args.empty() && args.front() == get_module_name()) {
                args.erase(args.begin());
            }

            return instance_.attr("module_autoComplete")(args).cast<std::vector<std::string>>();
        });
    }
};

class PythonSource final : public detail::ModuleSource {
    py::object type_;
public:
    Result<void> prepare(const std::vector<std::string>& args, const detail::RuntimeServices&) override {
        py::gil_scoped_acquire gil;
        try {
            auto path = py::module_::import("pathlib").attr("Path")(args[detail::bootstrap::plugin_path]);
            py::module_::import("sys").attr("path").attr("insert")(1, path.attr("parent").attr("__str__")());
            auto util = py::module_::import("importlib.util");
            auto spec = util.attr("spec_from_file_location")("_vinc_application", path);
            auto script = util.attr("module_from_spec")(spec);
            py::module_::import("sys").attr("modules")["_vinc_application"] = script;
            spec.attr("loader").attr("exec_module")(script);
            type_ = script.attr("Module");
            return {};
        } catch (py::error_already_set& error) {
            return std::unexpected(report_exception(error, ErrorOperation::LoadPython,
                args[detail::bootstrap::module_type], std::stoull(args[detail::bootstrap::generation])));
        }
    }

    Result<detail::RuntimeModule> create(std::string name) override {
        detail::RuntimeModule module(new PythonModule(std::move(name)), [](detail::ModuleObject* value) noexcept {
            delete static_cast<PythonModule*>(value);
        });
        auto initialized = static_cast<PythonModule*>(module.get())->initialize(type_);
        if (!initialized) {
            return std::unexpected(initialized.error());
        }

        return module;
    }

    bool fatal_command_errors() const noexcept override {
        return true;
    }

    void clear() noexcept override {
        py::gil_scoped_acquire gil;
        type_ = py::object{};
    }
};

void bind_errors(py::module_& module) {
    py::class_<NativeError>(module, "NativeError")
        .def_property_readonly("source", [](const NativeError& error) {
            return static_cast<std::uint32_t>(error.source);
        })
        .def_property_readonly("value", &NativeError::value);
    py::class_<ErrorRecord>(module, "ErrorRecord")
        .def_property_readonly("domain", [](const ErrorRecord& error) {
            return static_cast<std::uint32_t>(error.domain);
        })
        .def_readonly("code", &ErrorRecord::code)
        .def_readonly("operation", &ErrorRecord::operation)
        .def_readonly("native", &ErrorRecord::native);
    py::class_<Error>(module, "Error")
        .def_property_readonly("domain", [](const Error& error) {
            return static_cast<std::uint32_t>(error.domain);
        })
        .def_readonly("code", &Error::code)
        .def_readonly("operation", &Error::operation)
        .def_readonly("native", &Error::native)
        .def_property_readonly("causes", [](const Error& error) {
            return std::vector<ErrorRecord>(error.causes().begin(), error.causes().end());
        })
        .def_property_readonly("truncated", &Error::truncated);
    module.def("error_to_string", &error_to_string);
}

void bind_context(py::module_& module) {
    py::class_<PumpStats>(module, "PumpStats")
        .def_readonly("callbacks", &PumpStats::callbacks)
        .def_readonly("commands", &PumpStats::commands)
        .def_readonly("lost_messages", &PumpStats::lost_messages)
        .def_readonly("stopped", &PumpStats::stopped);
    py::class_<ContextState, std::shared_ptr<ContextState>>(module, "Context")
        .def("living", [](std::shared_ptr<ContextState> state) {
            ContextCall call(std::move(state));
            return call.context().living();
        })
        .def("get_mid", [](std::shared_ptr<ContextState> state) {
            ContextCall call(std::move(state));
            return call.module().get_mid();
        })
        .def("get_module_name", [](std::shared_ptr<ContextState> state) {
            ContextCall call(std::move(state));
            return call.module().get_module_name();
        })
        .def("pumpOnce", [](std::shared_ptr<ContextState> state) {
            ContextCall call(std::move(state));
            Result<PumpStats> result;
            {
                py::gil_scoped_release release;
                result = call.context().pumpOnce();
            }

            return unwrap(std::move(result));
        })
        .def("pumpLoop", [](std::shared_ptr<ContextState> state) {
            ContextCall call(std::move(state));
            Result<void> result;
            {
                py::gil_scoped_release release;
                result = call.context().pumpLoop();
            }

            unwrap(std::move(result));
        });
}
}

PYBIND11_MODULE(_native, module) {
    using namespace vinc::python;
    bind_errors(module);
    bind_context(module);
    bind_services(module);
    bind_messages(module);
    module.def("_run", [](std::vector<std::string> args) {
        // A process has exactly one admission/lifetime; never reconnect with a
        // different MID or reuse wrappers after the first runtime has retired.
        static std::atomic_flag started = ATOMIC_FLAG_INIT;
        if (started.test_and_set()) {
            throw py::value_error("VINC runtime already used in this process");
        }

        if (args.size() > vinc::detail::bootstrap::argument_count &&
            args[vinc::detail::bootstrap::argument_count] == args[vinc::detail::bootstrap::module_type]) {
            args.erase(args.begin() + vinc::detail::bootstrap::argument_count);
        }

        PythonSource source;
        py::gil_scoped_release release;
        return vinc::detail::run_module_runtime(args, source);
    });
}

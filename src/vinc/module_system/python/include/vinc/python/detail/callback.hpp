#pragma once
#include <vinc/python/detail/bridge.hpp>

namespace vinc::python {
// Native callbacks can retire on a thread without the GIL. Every reference
// decrement is guarded, and teardown explicitly breaks bound-method cycles.
class PythonCallback final : public PythonResource {
    py::object function_;
    std::weak_ptr<ContextState> state_;
    ErrorOperation operation_;

public:
    PythonCallback(py::object function, std::shared_ptr<ContextState> state, ErrorOperation operation)
        : function_(std::move(function)), state_(state), operation_(operation) {
        if (!PyCallable_Check(function_.ptr())) {
            throw py::type_error("callback must be callable");
        }
    }

    ~PythonCallback() override {
        py::gil_scoped_acquire gil;
        clear();
    }

    void clear() override {
        function_ = py::object{};
    }

    template<class Argument> void invoke(Argument&& argument) {
        py::gil_scoped_acquire gil;
        auto state = state_.lock();
        if (!state || !function_) {
            return;
        }

        ContextCall call(state);
        try {
            // Keep the active callable alive if it replaces itself in this call.
            py::object function = function_;
            if (!function(argument()).is_none()) {
                throw py::type_error("VINC callbacks return None");
            }
        } catch (const py::builtin_exception& error) {
            error.set_error();
            py::error_already_set converted;
            fail(call, converted);
        } catch (py::error_already_set& error) {
            fail(call, error);
        }
    }

private:
    void fail(ContextCall& call, py::error_already_set& exception) {
        auto error = report_exception(exception, operation_, call.module().get_module_name(), call.module().get_mid());
        error = wrap_error(ErrorCode::HookException, operation_, error);
        py::gil_scoped_release release;
        detail::ContextLanguageAccess::fail(call.context(), error);
    }
};

inline std::shared_ptr<PythonCallback> callback(py::object function,
    const std::shared_ptr<ContextState>& state, ErrorOperation operation) {
    auto result = std::make_shared<PythonCallback>(std::move(function), state, operation);
    state->track(result);
    return result;
}
}

#pragma once
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <vinc/module/detail/module_object.hpp>
#include <vinc/module/detail/context_runtime.hpp>
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include <vector>

namespace py = pybind11;
namespace vinc::python {
using module_system::ErrorCode;
using module_system::ErrorOperation;
struct PythonResource {
    virtual ~PythonResource() = default;
    virtual void clear() = 0;
};
[[noreturn]] inline void raise_error(const Error& error) {
    auto type = py::module_::import("vinc").attr("VincError");
    auto instance = type(py::cast(error));
    PyErr_SetObject(type.ptr(), instance.ptr());
    throw py::error_already_set();
}

template <class T> T unwrap(Result<T> result) {
    if (!result) {
        raise_error(result.error());
    }

    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

// Python wrappers may outlive native teardown. Short admission bookkeeping
// protects the pointer; no mutex is held while pumping or executing user code.
struct ContextState {
    std::mutex mutex;
    std::condition_variable changed;
    detail::ModuleObject* module = nullptr;
    Context* context = nullptr;
    std::size_t calls = 0;
    bool closing = false;
    std::vector<std::weak_ptr<PythonResource>> resources;

    // Resource registration and clearing require the GIL; native admission uses
    // the separate mutex so heartbeat/watchdog never need Python synchronization.
    void track(const std::shared_ptr<PythonResource>& resource) {
        std::erase_if(resources, [](const auto& value) {
            return value.expired();
        });
        resources.push_back(resource);
    }

    void revoke() {
        std::unique_lock lock(mutex);
        closing = true;
        changed.wait(lock, [&] {
            return calls == 0;
        });
        module = nullptr;
        context = nullptr;
    }
};

class ContextCall {
    std::shared_ptr<ContextState> state_;
public:
    explicit ContextCall(std::shared_ptr<ContextState> state) : state_(std::move(state)) {
        bool closed = false;
        {
            std::lock_guard lock(state_->mutex);
            closed = state_->closing || !state_->context;
            if (!closed) {
                ++state_->calls;
            }
        }

        if (closed) {
            raise_error(Error{ErrorCode::ResourcesUnavailable, ErrorOperation::Pump});
        }
    }
    ~ContextCall() {
        std::lock_guard lock(state_->mutex);
        --state_->calls;
        state_->changed.notify_all();
    }

    ContextCall(const ContextCall&) = delete;
    ContextCall& operator=(const ContextCall&) = delete;

    Context& context() const {
        return *state_->context;
    }

    detail::ModuleObject& module() const {
        return *state_->module;
    }
};


Error report_exception(py::error_already_set&, ErrorOperation, std::string_view, MID);
void bind_services(py::module_& module);
void bind_messages(py::module_& module);
void clear_resources(const std::shared_ptr<ContextState>& state);
}

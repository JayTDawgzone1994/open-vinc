#include <vinc/python/detail/bridge.hpp>
#include <vinc/module/detail/context_rpc.hpp>
#include <vinc/rpc/client.hpp>
#include <vinc/message/storage.hpp>

namespace vinc::python {
namespace {
struct RpcCallback final : PythonResource {
    std::weak_ptr<ContextState> state;
    py::object request_type;
    py::object response_type;
    py::object function;

    ~RpcCallback() override {
        py::gil_scoped_acquire gil;
        clear();
    }

    void clear() override {
        request_type = {};
        response_type = {};
        function = {};
    }

    Error fail(ContextCall& call, py::error_already_set& error) {
        auto failure = report_exception(error, ErrorOperation::Pump, call.module().get_module_name(),
                                        call.module().get_mid());
        auto wrapped = wrap_error(rpc::ErrorCode::CallbackFailed, rpc::ErrorOperation::Pump, failure);
        py::gil_scoped_release release;
        detail::ContextLanguageAccess::fail(call.context(),
                                            wrap_error(ErrorCode::RpcFailed, ErrorOperation::Pump, wrapped));
        return wrapped;
    }

    Result<rpc::detail::RawReply> handle(control::Bytes image, std::uint64_t limit) {
        py::gil_scoped_acquire gil;
        auto owner = state.lock();
        if (!owner || !function) {
            return std::unexpected(Error{rpc::ErrorCode::Closed, rpc::ErrorOperation::Call});
        }

        ContextCall call(owner);
        try {
            auto request = request_type();
            auto response = response_type();
            // Python bytes consumes byte-addressable encoded storage, never C++ object layout.
            try {
                request.attr("deserialize")(
                    py::bytes(reinterpret_cast<const char*>(image.data()), image.size()));
            } catch (py::error_already_set& error) {
                if (py::hasattr(error.value(), "error")) {
                    return std::unexpected(wrap_error(rpc::ErrorCode::StorageFailed,
                                                      rpc::ErrorOperation::Receive,
                                                      py::cast<Error>(error.value().attr("error"))));
                }

                throw;
            }

            auto returned = function(request, response);
            if (!py::isinstance<rpc::RpcStatus>(returned)) {
                throw py::type_error("RPC handlers return vinc.rpc.RpcStatus");
            }

            auto status = py::cast<rpc::RpcStatus>(returned);
            py::bytes encoded;
            try {
                encoded = py::cast<py::bytes>(response.attr("_rpc_snapshot")(limit));
            } catch (py::error_already_set& error) {
                if (py::hasattr(error.value(), "error")) {
                    return std::unexpected(wrap_error(rpc::ErrorCode::StorageFailed,
                                                      rpc::ErrorOperation::Send,
                                                      py::cast<Error>(error.value().attr("error"))));
                }

                throw;
            }

            auto bytes = py::cast<std::string>(encoded);
            if (bytes.size() > limit) {
                return std::unexpected(Error{rpc::ErrorCode::CapacityExceeded, rpc::ErrorOperation::Send});
            }

            auto snapshot =
                message::Snapshot::copy_from(std::as_bytes(std::span(bytes.data(), bytes.size())));
            if (!snapshot) {
                return std::unexpected(
                    wrap_error(rpc::ErrorCode::StorageFailed, rpc::ErrorOperation::Send, snapshot.error()));
            }

            return rpc::detail::RawReply{std::move(status), std::move(*snapshot)};
        } catch (const py::builtin_exception& error) {
            error.set_error();
            py::error_already_set converted;
            return std::unexpected(fail(call, converted));
        } catch (py::error_already_set& error) {
            return std::unexpected(fail(call, error));
        }
    }

    void complete(Result<rpc::detail::RawResult> result) {
        py::gil_scoped_acquire gil;
        auto owner = state.lock();
        if (!owner || !function) {
            return;
        }

        ContextCall call(owner);
        try {
            // Async errors are values in the completion; raising on the receiver
            // thread would neither reach the caller nor preserve pump ownership.
            py::object reply = py::none();
            py::object error = py::none();
            if (!result) {
                error = py::module_::import("vinc").attr("VincError")(py::cast(result.error()));
            } else {
                auto response = response_type();
                // Byte view is valid throughout the Python copy.
                try {
                    response.attr("deserialize")(
                        py::bytes(reinterpret_cast<const char*>(result->image.data()), result->image.size()));
                    reply = py::make_tuple(result->status, response);
                } catch (py::error_already_set& failure) {
                    if (!py::hasattr(failure.value(), "error")) {
                        throw;
                    }

                    auto cause = py::cast<Error>(failure.value().attr("error"));
                    error = py::module_::import("vinc").attr("VincError")(py::cast(
                        wrap_error(rpc::ErrorCode::StorageFailed, rpc::ErrorOperation::Receive, cause)));
                }
            }

            if (!function(reply, error).is_none()) {
                throw py::type_error("RPC completion callbacks return None");
            }
        } catch (const py::builtin_exception& error) {
            error.set_error();
            py::error_already_set converted;
            (void)fail(call, converted);
        } catch (py::error_already_set& error) {
            (void)fail(call, error);
        }
    }
};

std::shared_ptr<RpcCallback> retained(const std::shared_ptr<ContextState>& state, py::object request,
                                      py::object response, py::object function) {
    for (auto type : {request, response}) {
        if (!PyType_Check(type.ptr()) || !py::hasattr(type, "_advertise") ||
            !py::hasattr(type, "schema_hash")) {
            throw py::type_error("RPC requires compiled VINC message types");
        }
    }

    if (!PyCallable_Check(function.ptr())) {
        throw py::type_error("RPC callback must be callable");
    }

    auto value = std::make_shared<RpcCallback>();
    value->state = state;
    value->request_type = std::move(request);
    value->response_type = std::move(response);
    value->function = std::move(function);
    state->track(value);
    return value;
}
}

void bind_rpc(py::module_& module) {
    auto rpc_module = module.def_submodule("rpc");
    py::class_<rpc::RpcStatus>(rpc_module, "RpcStatus")
        .def_static("success", &rpc::RpcStatus::success)
        .def_static("failure", &rpc::RpcStatus::failure, py::arg("code"), py::arg("reason") = "")
        .def_readonly("code", &rpc::RpcStatus::code)
        .def_readonly("reason", &rpc::RpcStatus::reason);
    py::class_<rpc::ServiceOptions>(rpc_module, "ServiceOptions")
        .def(py::init<>())
        .def_readwrite("request_bytes", &rpc::ServiceOptions::request_bytes)
        .def_readwrite("response_bytes", &rpc::ServiceOptions::response_bytes)
        .def_readwrite("slots", &rpc::ServiceOptions::slots);
    auto context = py::reinterpret_borrow<py::class_<ContextState, std::shared_ptr<ContextState>>>(
        module.attr("Context"));
    py::class_<rpc::Stats>(rpc_module, "Stats")
        .def_readonly("pending", &rpc::Stats::pending)
        .def_readonly("request_bytes", &rpc::Stats::request_bytes)
        .def_readonly("queue_full", &rpc::Stats::queue_full)
        .def_readonly("timeouts", &rpc::Stats::timeouts)
        .def_readonly("requests", &rpc::Stats::requests)
        .def_readonly("completions", &rpc::Stats::completions);
    context.def("rpc_stats", [](std::shared_ptr<ContextState> state) {
        ContextCall call(state);
        py::gil_scoped_release release;
        return call.context().rpc_stats();
    });
    context.def(
        "register_rpc",
        [](std::shared_ptr<ContextState> state, py::object request_type, py::object response_type,
           std::string name, py::object function, rpc::ServiceOptions options) {
            ContextCall call(state);
            auto callback = retained(state, request_type, response_type, std::move(function));
            auto request_hash = py::cast<std::uint64_t>(request_type.attr("schema_hash"));
            auto response_hash = py::cast<std::uint64_t>(response_type.attr("schema_hash"));
            Result<void> result;
            {
                py::gil_scoped_release release;
                result = rpc::detail::ClientAccess::register_raw(
                    detail::ContextRpcAccess::client(call.context()), std::move(name), request_hash,
                    response_hash, options, [callback](control::Bytes bytes, std::uint64_t maximum) {
                        return callback->handle(std::move(bytes), maximum);
                    });
            }

            unwrap(std::move(result));
        },
        py::arg("request_type"), py::arg("response_type"), py::arg("name"), py::arg("handler"),
        py::arg("options") = rpc::ServiceOptions{});
    context.def(
        "call_rpc",
        [](std::shared_ptr<ContextState> state, py::object request_type, py::object response_type,
           std::string target, std::string service, py::object request, py::object function,
           std::int64_t timeout_ms) {
            ContextCall call(state);
            auto callback = retained(state, request_type, response_type, std::move(function));
            if (!py::isinstance(request, request_type)) {
                throw py::type_error("request does not match its declared RPC type");
            }

            auto request_hash = py::cast<std::uint64_t>(request_type.attr("schema_hash"));
            auto response_hash = py::cast<std::uint64_t>(response_type.attr("schema_hash"));
            auto bytes = py::cast<std::string>(request.attr("_rpc_snapshot")(config::rpc_local_bytes));
            if (bytes.size() > config::rpc_local_bytes) {
                raise_error(Error{rpc::ErrorCode::CapacityExceeded, rpc::ErrorOperation::Call});
            }

            Result<void> result;
            {
                py::gil_scoped_release release;
                auto snapshot =
                    message::Snapshot::copy_from(std::as_bytes(std::span(bytes.data(), bytes.size())));
                if (!snapshot) {
                    result = std::unexpected(wrap_error(rpc::ErrorCode::StorageFailed,
                                                        rpc::ErrorOperation::Call, snapshot.error()));
                } else {
                    result = rpc::detail::ClientAccess::call_raw(
                        detail::ContextRpcAccess::client(call.context()), std::move(target),
                        std::move(service), request_hash, response_hash, std::move(*snapshot),
                        {std::chrono::milliseconds(timeout_ms)},
                        [callback](Result<rpc::detail::RawResult> reply) {
                            callback->complete(std::move(reply));
                        });
                }
            }

            unwrap(std::move(result));
        },
        py::arg("request_type"), py::arg("response_type"), py::arg("target"), py::arg("service"),
        py::arg("request"), py::arg("completion"),
        py::arg("timeout_ms") =
            std::chrono::duration_cast<std::chrono::milliseconds>(config::rpc_call_timeout).count());
}
}

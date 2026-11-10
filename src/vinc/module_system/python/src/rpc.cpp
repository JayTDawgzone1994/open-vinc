#include <vinc/python/detail/bridge.hpp>
#include <vinc/module/detail/context_rpc.hpp>
#include <vinc/rpc/client.hpp>
#include <vinc/rpc/detail/async_call.hpp>
#include <vinc/message/storage.hpp>

namespace vinc::python {
namespace {
void validate_types(const py::object& request, const py::object& response) {
    for (auto type : {request, response}) {
        if (!PyType_Check(type.ptr()) || !py::hasattr(type, "_advertise") || !py::hasattr(type, "schema_hash")) {
            throw py::type_error("RPC requires compiled VINC message types");
        }
    }
}

Result<message::Snapshot> snapshot(const py::object& request) {
    try {
        auto bytes = py::cast<std::string>(request.attr("_rpc_snapshot")(config::rpc_local_bytes));
        return message::Snapshot::copy_from(std::as_bytes(std::span(bytes.data(), bytes.size())));
    } catch (py::error_already_set& failure) {
        if (!py::hasattr(failure.value(), "error")) {
            throw;
        }

        return std::unexpected(py::cast<Error>(failure.value().attr("error")));
    }
}

struct RpcMessages {
    py::object request_type;
    py::object response_type;
    py::object request;
    py::object response;
    RpcMessages(py::object request_class, py::object response_class)
        : request_type(std::move(request_class)), response_type(std::move(response_class)) {
        validate_types(request_type, response_type);
        request = request_type();
        response = response_type();
    }
};

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

            py::object active_function = function;
            if (!active_function(reply, error).is_none()) {
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
    validate_types(request, response);

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
struct RpcClient {
    std::weak_ptr<ContextState> state;
    py::object request_type;
    rpc::detail::AsyncCall call;

    void submit(py::object request, std::optional<rpc::CallOptions> options) {
        auto owner = state.lock();
        if (!owner) {
            raise_error(Error{rpc::ErrorCode::Closed, rpc::ErrorOperation::Call});
        }
        ContextCall entered(owner);
        if (!py::isinstance(request, request_type)) {
            throw py::type_error("request does not match its declared RPC type");
        }
        // Python serialization needs the GIL. Async admission does not wait for
        // callbacks; completion dispatch on another pump acquires the GIL later.
        auto make_image = [&]() {
            return snapshot(request);
        };
        unwrap(options ? call.call(make_image, *options) : call.call(make_image));
    }
};

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
    py::class_<rpc::CallOptions>(rpc_module, "CallOptions")
        .def(py::init<>())
        .def_property("timeout_ms",
            [](const rpc::CallOptions& options) {
                return options.timeout.count();
            },
            [](rpc::CallOptions& options, std::int64_t value) {
                options.timeout = std::chrono::milliseconds(value);
            });
    py::class_<RpcMessages>(rpc_module, "RpcMessages")
        .def(py::init<py::object, py::object>())
        .def("request", [](RpcMessages& messages) {
            return messages.request;
        })
        .def("response", [](RpcMessages& messages) {
            return messages.response;
        });
    py::class_<RpcClient, std::shared_ptr<RpcClient>>(rpc_module, "RpcClient")
        .def("make_request", [](RpcClient& client) {
            return client.request_type();
        })
        .def("close", [](RpcClient& client) {
            client.call.close();
        })
        .def("call", &RpcClient::submit, py::arg("request"), py::arg("options") = py::none());
    context.def("rpc_client",
        [](std::shared_ptr<ContextState> state, py::object request_type, py::object response_type,
           std::string address, py::object function, rpc::CallOptions options) {
            ContextCall entered(state);
            if (!entered.context().living()) {
                raise_error(Error{rpc::ErrorCode::Closed, rpc::ErrorOperation::Create});
            }

            auto callback = retained(state, request_type, response_type, std::move(function));
            auto client = std::make_shared<RpcClient>();
            client->state = state;
            client->request_type = request_type;
            client->call = unwrap(rpc::detail::AsyncCall::create(
                rpc::detail::ClientAccess::endpoint(detail::ContextRpcAccess::client(entered.context())),
                address, py::cast<std::uint64_t>(request_type.attr("schema_hash")),
                py::cast<std::uint64_t>(response_type.attr("schema_hash")),
                [callback](Result<rpc::detail::RawResult> reply) {
                    callback->complete(std::move(reply));
                }, options));
            return client;
        }, py::arg("request_type"), py::arg("response_type"), py::arg("address"),
        py::arg("completion"), py::arg("options") = rpc::CallOptions{});
    context.def("call_rpc",
        [](std::shared_ptr<ContextState> state, std::string address, RpcMessages& messages,
           rpc::CallOptions options) {
            ContextCall entered(state);
            auto target = unwrap(detail::ContextRpcAccess::prepare(entered.context(), address));
            if (options.timeout.count() <= 0 || options.timeout > config::rpc_max_call_timeout) {
                raise_error(Error{rpc::ErrorCode::InvalidOptions, rpc::ErrorOperation::Call});
            }

            const auto deadline = std::chrono::steady_clock::now() + options.timeout;
            auto image = snapshot(messages.request);
            if (!image) {
                raise_error(wrap_error(rpc::ErrorCode::StorageFailed, rpc::ErrorOperation::Call, image.error()));
            }

            const auto request_hash = py::cast<std::uint64_t>(messages.request_type.attr("schema_hash"));
            const auto response_hash = py::cast<std::uint64_t>(messages.response_type.attr("schema_hash"));
            Result<rpc::detail::RawResult> result;
            {
                py::gil_scoped_release release;
                result = rpc::detail::ClientAccess::call_sync(
                    detail::ContextRpcAccess::client(entered.context()), target,
                    request_hash, response_hash, std::move(*image), options, deadline);
            }

            auto value = unwrap(std::move(result));
            auto response = messages.response_type();
            try {
                // Serialized bytes are a copied image, never a Python/C++ object layout.
                response.attr("deserialize")(
                    py::bytes(reinterpret_cast<const char*>(value.image.data()), value.image.size()));
            } catch (py::error_already_set& failure) {
                if (!py::hasattr(failure.value(), "error")) {
                    throw;
                }

                raise_error(wrap_error(rpc::ErrorCode::StorageFailed, rpc::ErrorOperation::Receive,
                                      py::cast<Error>(failure.value().attr("error"))));
            }

            messages.response = std::move(response);
            return value.status;
        }, py::arg("address"), py::arg("messages"), py::arg("options") = rpc::CallOptions{});
}
}

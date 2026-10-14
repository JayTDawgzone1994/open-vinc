#include <vinc/module/base_module.hpp>
#include <vinc_messages/Frame.hpp>
#include <vinc_messages/Detection.hpp>
#include <thread>
#include <stdexcept>
#include <iostream>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto test_limit = 12s;
constexpr auto poll = 5ms;
constexpr auto slow_handler = 250ms;
constexpr auto timeout_limit = 100ms;
constexpr auto late_reply_drain = 1s;
constexpr std::uint64_t image_bytes = 1024 * 1024;

void check(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

template <class T> auto take(Result<T> value) {
    if (!value) {
        throw std::runtime_error(error_to_string(value.error()));
    }

    if constexpr (!std::is_void_v<T>) {
        return std::move(*value);
    }
}

class RpcModule final : public BaseModule<RunMode::Manual> {
    std::string mode_;
    std::uint64_t served_ = 0;
    std::vector<std::uint64_t> order_;

    void until(const std::function<bool()>& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + test_limit;
        while (!predicate() && context.living() && std::chrono::steady_clock::now() < deadline) {
            take(context.pumpOnce());
            std::this_thread::sleep_for(poll);
        }

        check(predicate(), "RPC wait completed");
    }

    void caller() {
        auto request = make_message<Frame>();
        request->data().sequence = 42;
        take(request->data().payload.resize(image_bytes));
        request->data().payload[image_bytes - 1] = 123;
        bool done = false;
        take((context.call_rpc<Frame, Frame>(
            "rpcServer", "echo", *request, {}, [&](Result<rpc::RpcReply<Frame>> reply) {
                auto value = take(std::move(reply));
                check(value.status.code == 0, "successful status");
                check(value.response->data().sequence.get() == 42, "owned request snapshot");
                check(value.response->data().payload.at(image_bytes - 1) == Result<std::uint8_t>{123},
                      "large payload");
                done = true;
            })));
        request->data().sequence = 999;
        until([&] {
            return done;
        });
        auto expect_failure = [&](const char* service, rpc::ErrorCode code) {
            done = false;
            take((context.call_rpc<Frame, Frame>(
                "rpcServer", service, *request, {}, [&](Result<rpc::RpcReply<Frame>> reply) {
                    check(!reply && reply.error().is(code), "expected recoverable RPC failure");
                    done = true;
                })));
            until([&] {
                return done;
            });
        };
        expect_failure("missing", rpc::ErrorCode::NotFound);
        expect_failure("largeResponse", rpc::ErrorCode::StorageFailed);
        expect_failure("longReason", rpc::ErrorCode::CapacityExceeded);
        take(request->data().payload.resize(3 * image_bytes));
        expect_failure("echo", rpc::ErrorCode::CapacityExceeded);
        take(request->data().payload.resize(image_bytes));
        done = false;
        take((context.call_rpc<Frame, Frame>(
            "rpcServer", "failure", *request, {}, [&](Result<rpc::RpcReply<Frame>> reply) {
                auto value = take(std::move(reply));
                check(value.status.code == 7 && value.status.reason == "device unavailable",
                      "application reason");
                check(value.response->data().sequence.get() == 17, "failure still returns response");
                done = true;
            })));
        until([&] {
            return done;
        });
        done = false;
        take((context.call_rpc<Frame, Detection>(
            "rpcServer", "echo", *request, {}, [&](Result<rpc::RpcReply<Detection>> reply) {
                check(!reply && reply.error().is(rpc::ErrorCode::TypeMismatch), "schema mismatch");
                done = true;
            })));
        until([&] {
            return done;
        });
        std::uint32_t full = 0;
        std::uint32_t success = 0;
        for (std::uint32_t index = 0; index < 6; ++index) {
            request->data().sequence = index;
            take((context.call_rpc<Frame, Frame>(
                "rpcServer", "slow", *request, {}, [&](Result<rpc::RpcReply<Frame>> reply) {
                    if (reply) {
                        ++success;
                    } else {
                        check(reply.error().is(rpc::ErrorCode::QueueFull), "full rejects without crashing");
                        ++full;
                    }
                })));
        }

        until([&] {
            return full + success == 6;
        });
        check(full >= 4 && success > 0, "fixed slots enforce admission");
        std::uint32_t notified = 0;
        take((context.call_rpc<Frame, Frame>(
            "rpcServer", "slow", *request, {timeout_limit}, [&](Result<rpc::RpcReply<Frame>> reply) {
                check(!reply && reply.error().is(rpc::ErrorCode::TimeoutOutcomeUnknown), "timeout outcome");
                ++notified;
            })));
        until([&] {
            return notified != 0;
        });
        const auto drain = std::chrono::steady_clock::now() + late_reply_drain;
        while (std::chrono::steady_clock::now() < drain) {
            take(context.pumpOnce());
            std::this_thread::sleep_for(poll);
        }

        check(notified == 1, "late reply does not notify twice");
        done = false;
        take((context.call_rpc<Frame, Frame>("rpcServer", "slow", *request, {},
                                             [&](Result<rpc::RpcReply<Frame>> reply) {
                                                 take(std::move(reply));
                                                 done = true;
                                             })));
        until([&] {
            return done;
        });
        std::cout << "RPC_NATIVE_PASSED\n";
    }

public:
    RpcModule() : BaseModule("rpcNode") {
    }

    Result<std::string> on_name(std::vector<std::string>& args) override {
        mode_ = args.back();
        return mode_ == "server" ? "rpcServer" : "rpcCaller";
    }

    Result<void> on_start(const std::vector<std::string>&) override {
        if (mode_ != "server") {
            return {};
        }

        rpc::ServiceOptions options{2 * image_bytes, 2 * image_bytes, 2};
        take((context.register_rpc<Frame, Frame>("largeResponse", options, [](const Frame&, Frame& response) {
            take(response.data().payload.resize(3 * image_bytes));
            return rpc::RpcStatus::success();
        })));
        take((context.register_rpc<Frame, Frame>("longReason", options, [](const Frame&, Frame&) {
            return rpc::RpcStatus::failure(7, std::string(config::rpc_reason_bytes + 1, 'x'));
        })));
        take(
            (context.register_rpc<Frame, Frame>("throw", options, [](const Frame&, Frame&) -> rpc::RpcStatus {
                throw std::runtime_error("intentional RPC handler failure");
            })));
        take((context.register_rpc<Frame, Frame>("echo", options, [&](const Frame& request, Frame& response) {
            ++served_;
            take(response.assign(request));
            return rpc::RpcStatus::success();
        })));
        take((context.register_rpc<Frame, Frame>("slow", options, [&](const Frame& request, Frame& response) {
            order_.push_back(request.data().sequence.get());
            std::this_thread::sleep_for(slow_handler);
            take(response.assign(request));
            return rpc::RpcStatus::success();
        })));
        take(
            (context.register_rpc<Frame, Frame>("crash", options, [](const Frame&, Frame&) -> rpc::RpcStatus {
                std::_Exit(86);
            })));
        return context.register_rpc<Frame, Frame>("failure", options, [](const Frame&, Frame& response) {
            response.data().sequence = 17;
            return rpc::RpcStatus::failure(7, "device unavailable");
        });
    }

    Result<void> main() override {
        if (mode_ == "server") {
            return context.pumpLoop();
        }

        if (mode_ == "target-exit" || mode_ == "target-throw") {
            auto request = make_message<Frame>();
            bool completed = false;
            const auto service = mode_ == "target-exit" ? "crash" : "throw";
            take((context.call_rpc<Frame, Frame>(
                "rpcServer", service, *request, {}, [&](Result<rpc::RpcReply<Frame>> result) {
                    check(!result && result.error().is(rpc::ErrorCode::TargetExited),
                          "confirmed target exit");
                    completed = true;
                })));
            until([&] {
                return completed;
            });
            return {};
        }

        caller();
        return {};
    }

    Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return static_cast<std::int32_t>(served_);
    }
};
}

REGISTER_MODULE(RpcModule, "rpcFixture")

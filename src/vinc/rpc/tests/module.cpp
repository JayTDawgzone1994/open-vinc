#include <vinc/module/base_module.hpp>
#include <vinc_messages/Frame.hpp>
#include <vinc_messages/Detection.hpp>
#include <thread>
#include <stdexcept>
#include <iostream>
#include <future>
#include "async_support.hpp"

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
    rpc_test::Clients clients_;
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
        take((clients_.send<Frame, Frame>(context,
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
            take((clients_.send<Frame, Frame>(context,
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
        take((clients_.send<Frame, Frame>(context,
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
        take((clients_.send<Frame, Detection>(context,
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
            take((clients_.send<Frame, Frame>(context,
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
        take((clients_.send<Frame, Frame>(context,
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
        take((clients_.send<Frame, Frame>(context, "rpcServer", "slow", *request, {},
                                             [&](Result<rpc::RpcReply<Frame>> reply) {
                                                 take(std::move(reply));
                                                 done = true;
                                             })));
        until([&] {
            return done;
        });
        std::cout << "RPC_NATIVE_PASSED\n";
    }

    void reusable_clients() {
        rpc::RpcClient<Frame, Frame> client;
        std::uint64_t replies = 0;
        std::vector<std::unique_ptr<Frame>> retained;
        auto request = make_message<Frame>();
        request->data().sequence = 42;
        client = take(context.rpc_client<Frame, Frame>("rpcServer.echo", [&](auto reply) {
            auto value = take(std::move(reply));
            retained.push_back(std::move(value.response));
            ++replies;
            if (replies < 3) {
                request->data().sequence = 42 + replies;
                take(client.call(*request)); // Busy must be cleared before callback entry.
            }
        }));
        take(client.call(*request));
        auto busy = client.call(*request);
        check(!busy && busy.error().is(rpc::ErrorCode::Busy), "one pending per real client");
        request->data().sequence = 999;
        until([&] {
            return replies == 3;
        });
        check(retained[0]->data().sequence.get() == 42 && retained[2]->data().sequence.get() == 44,
              "snapshots and retained replies remain independent");

        auto closed = take(context.rpc_client<Frame, Frame>("rpcServer.echo", [](auto) {
            throw std::runtime_error("closed callback dispatched");
        }));
        take(closed.call(*request));
        closed.close();
        until([&] {
            return context.rpc_stats().pending == 0;
        });

        bool done = false;
        auto self = take(context.rpc_client<Frame, Frame>("rpcCaller.echo", [&](auto reply) {
            take(std::move(reply));
            done = true;
        }));
        take(self.call(*request));
        until([&] {
            return done;
        });
    }

    void synchronous_caller() {
        auto messages = make_rpc_messages<Frame, Frame>();
        messages.request().data().sequence = 42;
        take(messages.request().data().payload.resize(image_bytes));
        messages.request().data().payload[image_bytes - 1] = 123;
        // No caller pump is running: transport reception belongs to the Host.
        auto status = take(context.call_rpc("rpcServer.echo", messages));
        check(status.code == 0 && messages.response().data().sequence.get() == 42, "sync response");
        check(messages.response().data().payload.at(image_bytes - 1) == Result<std::uint8_t>{123}, "sync large image");
        messages.request().data().sequence = 43;
        take(context.call_rpc("rpcServer.echo", messages));
        check(messages.response().data().sequence.get() == 43, "message pair reused");
        status = take(context.call_rpc("rpcServer.failure", messages));
        check(status.code == 7 && status.reason == "device unavailable", "sync application failure");
        check(messages.response().data().sequence.get() == 17, "application failure response retained");
        auto failed = context.call_rpc("rpcServer.missing", messages);
        check(!failed && failed.error().is(rpc::ErrorCode::NotFound), "sync missing service");
        check(messages.response().data().sequence.get() == 17, "runtime error preserves response");
        auto mismatch = make_rpc_messages<Frame, Detection>();
        auto wrong_type = context.call_rpc("rpcServer.echo", mismatch);
        check(!wrong_type && wrong_type.error().is(rpc::ErrorCode::TypeMismatch), "sync schema mismatch");
        auto self = context.call_rpc("rpcCaller.echo", messages);
        check(!self && self.error().is(rpc::ErrorCode::SynchronousSelfCall), "sync self rejected");
        auto guarded = take(context.call_rpc("rpcServer.guard", messages));
        check(guarded.code == 0, "service callback rejects blocking calls before admission");

        auto timeout = context.call_rpc("rpcServer.slow", messages, {timeout_limit});
        check(!timeout && timeout.error().is(rpc::ErrorCode::TimeoutOutcomeUnknown), "sync timeout");
        check(messages.response().data().sequence.get() == 17, "timeout preserves response");
        const auto deadline = std::chrono::steady_clock::now() + test_limit;
        while (context.rpc_stats().pending && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(poll); // Deliberately never pump late cleanup.
        }
        check(context.rpc_stats().pending == 0, "Host retires late sync reply without pump");

        // A pump on another thread must not reject this caller. The same Context
        // owns both threads, but only the actual pump thread is forbidden to block.
        std::jthread pump([&](std::stop_token stop) {
            while (!stop.stop_requested() && context.living()) {
                take(context.pumpOnce());
                std::this_thread::sleep_for(poll);
            }
        });
        take(context.call_rpc("rpcServer.echo", messages));
        pump.request_stop();
        pump.join();

        std::vector<std::future<void>> callers;
        constexpr std::uint64_t parallel_callers = 4;
        for (std::uint64_t index = 0; index < parallel_callers; ++index) {
            callers.push_back(std::async(std::launch::async, [&, index] {
                auto pair = make_rpc_messages<Frame, Frame>();
                pair.request().data().sequence = index;
                take(context.call_rpc("rpcServer.echo", pair));
                check(pair.response().data().sequence.get() == index, "concurrent replies stay with their waiter");
            }));
        }

        bool done = false;
        take((clients_.send<Frame, Frame>(context, "rpcServer", "echo", messages.request(), {},
            [&](Result<rpc::RpcReply<Frame>> reply) {
                take(std::move(reply));
                auto blocked = context.call_rpc("rpcServer.echo", messages);
                check(!blocked && blocked.error().is(rpc::ErrorCode::BlockingCallInPump),
                      "completion callback rejects sync");
                // Async calls remain legal from that exact same callback.
                take((clients_.send<Frame, Frame>(context, "rpcServer", "echo", messages.request(), {},
                    [&](Result<rpc::RpcReply<Frame>> next) {
                        take(std::move(next));
                        done = true;
                    })));
            })));
        until([&] {
            return done;
        });
        for (auto& caller : callers) {
            caller.get();
        }
        take(context.call_rpc("rpcServer.echo", messages)); // Guard restored after pump.
        std::cout << "RPC_SYNC_PASSED\n";
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
            return context.register_rpc<Frame, Frame>("echo", [](const Frame& request, Frame& response) {
                take(response.assign(request));
                return rpc::RpcStatus::success();
            });
        }

        rpc::ServiceOptions options{2 * image_bytes, 2 * image_bytes, 2};
        take((context.register_rpc<Frame, Frame>("guard", [&](const Frame&, Frame& response) {
            auto messages = make_rpc_messages<Frame, Frame>();
            const auto before = context.rpc_stats().pending;
            auto blocked = context.call_rpc("rpcCaller.missing", messages);
            check(!blocked && blocked.error().is(rpc::ErrorCode::BlockingCallInPump), "handler guard");
            check(context.rpc_stats().pending == before, "guard has no admission side effects");
            response.data().sequence = 17;
            return rpc::RpcStatus::success();
        }, options)));
        take((context.register_rpc<Frame, Frame>("largeResponse", [](const Frame&, Frame& response) {
            take(response.data().payload.resize(3 * image_bytes));
            return rpc::RpcStatus::success();
        }, options)));
        take((context.register_rpc<Frame, Frame>("longReason", [](const Frame&, Frame&) {
            return rpc::RpcStatus::failure(7, std::string(config::rpc_reason_bytes + 1, 'x'));
        }, options)));
        take(
            (context.register_rpc<Frame, Frame>("throw", [](const Frame&, Frame&) -> rpc::RpcStatus {
                throw std::runtime_error("intentional RPC handler failure");
            }, options)));
        auto echo_options = options;
        echo_options.slots = 8; // Accommodate concurrent sync callers plus the async regression.
        take((context.register_rpc<Frame, Frame>("echo", [&](const Frame& request, Frame& response) {
            ++served_;
            take(response.assign(request));
            return rpc::RpcStatus::success();
        }, echo_options)));
        take((context.register_rpc<Frame, Frame>("slow", [&](const Frame& request, Frame& response) {
            order_.push_back(request.data().sequence.get());
            std::this_thread::sleep_for(slow_handler);
            take(response.assign(request));
            return rpc::RpcStatus::success();
        }, options)));
        take(
            (context.register_rpc<Frame, Frame>("crash", [](const Frame&, Frame&) -> rpc::RpcStatus {
                std::_Exit(86);
            }, options)));
        return context.register_rpc<Frame, Frame>("failure", [](const Frame&, Frame& response) {
            response.data().sequence = 17;
            return rpc::RpcStatus::failure(7, "device unavailable");
        }, options);
    }

    Result<void> main() override {
        if (mode_ == "server") {
            return context.pumpLoop();
        }

        if (mode_ == "target-exit" || mode_ == "target-throw") {
            auto request = make_message<Frame>();
            bool completed = false;
            const auto service = mode_ == "target-exit" ? "crash" : "throw";
            take((clients_.send<Frame, Frame>(context,
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

        if (mode_ == "sync") {
            reusable_clients();
            synchronous_caller();
            return {};
        }
        if (mode_ == "sync-exit") {
            auto messages = make_rpc_messages<Frame, Frame>();
            auto result = context.call_rpc("rpcServer.crash", messages);
            check(!result && result.error().is(rpc::ErrorCode::TargetExited), "sync target exit wakes caller");
            return {};
        }

        caller();
        return {};
    }

    Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args.front() == "guard") {
            auto messages = make_rpc_messages<Frame, Frame>();
            auto result = context.call_rpc("rpcCaller.missing", messages);
            check(!result && result.error().is(rpc::ErrorCode::BlockingCallInPump), "Host to Plugin command guard");
            return 0;
        }
        return static_cast<std::int32_t>(served_);
    }
};
}

REGISTER_MODULE(RpcModule, "rpcFixture")

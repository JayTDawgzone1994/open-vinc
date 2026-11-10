#include <vinc/module/base_module.hpp>
#include <vinc_messages/Frame.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto gate_timeout = 8s;
constexpr auto gate_poll = 2ms;

void check(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
template<class T>
auto take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

// Gates belong to this disposable test process. They never depend on on_stop
// releasing a callback, and the parent's timeout still bounds a broken fixture.
class ParallelStopModule final : public BaseModule<RunMode::Manual> {
    std::string mode_;
    std::filesystem::path directory_;
    std::optional<message_bus::Publisher<Frame>> publisher_;
    std::optional<message_bus::Subscriber<Frame>> subscriber_;
    std::optional<message_bus::Subscriber<Frame>> replacement_;
    std::jthread pump_;
    std::atomic<bool> active_{false};
    std::atomic<bool> entered_{false};
    std::atomic<bool> replaced_{false};
    std::atomic<bool> pump_returned_{false};
    std::atomic<int> calls_{0};

    void mark(const char* name) {
        std::ofstream file(directory_ / name);
        file << "ready\n";
        check(bool(file), "cannot write lifecycle marker");
    }
    template<class Predicate>
    void until(Predicate ready) {
        const auto deadline = std::chrono::steady_clock::now() + gate_timeout;
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(gate_poll);
        }
        check(ready(), "lifecycle gate timed out");
    }
    void receive(std::unique_ptr<Frame>) {
        check(!active_.exchange(true), "old and replacement Topic overlapped");
        ++calls_;
        if (mode_ == "replace") {
            subscriber_->close();
            replacement_.emplace(take(context.subscribe<Frame>("stop/topic", [this](auto) {
                check(!active_, "replacement bypassed active Topic owner");
                replaced_ = true;
            }, {.start = message_bus::StartPosition::NewOnly})));
            auto next = make_message<Frame>();
            take(publisher_->push(*next));
        }
        mark("entered");
        entered_ = true;
        until([&] { return std::filesystem::exists(directory_ / "release"); });
        active_ = false;
        if (mode_ == "throw") {
            throw std::runtime_error("intentional callback fault during stop drain");
        }
    }
public:
    ParallelStopModule() : BaseModule("parallelStop") {
    }
    Result<void> on_start(const std::vector<std::string>& args) override {
        mode_ = args.at(0);
        directory_ = args.at(1);
        publisher_.emplace(take(context.advertise<Frame>("stop/topic", {.slots = 8, .max_message_bytes = 8192})));
        subscriber_.emplace(take(context.subscribe<Frame>("stop/topic", [this](auto message) {
            receive(std::move(message));
        })));
        auto message = make_message<Frame>();
        take(publisher_->push(*message));
        take(publisher_->push(*message));
        return {};
    }
    Result<void> main() override {
        pump_ = std::jthread([this] {
            // Deliberately ignore failure: Host must latch a worker exception.
            (void)context.pumpLoop({.topic_workers = 2});
            pump_returned_ = true;
        });
        until([&] { return entered_.load(); });
        // main is an application thread, not a dispatcher. A missing remote
        // target must reach ordinary RPC routing even while a Topic is active.
        auto messages = make_rpc_messages<Frame, Frame>();
        auto absent = context.call_rpc("missing.echo", messages);
        check(!absent && absent.error().is(rpc::ErrorCode::NotFound),
              "application thread was incorrectly classified as a callback");
        if (mode_ == "replace") {
            until([&] { return replaced_.load(); });
        }
        return {};
    }
    Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }
    Result<void> on_stop() override {
        mark("stop_entered");
        check(!active_, "on_stop raced active callback");
        check(calls_ == 1, "queued callback ran after stop or self-close");
        if (pump_.joinable()) {
            pump_.join();
        }
        check(pump_returned_, "application pump thread did not return");
        mark("cleaned");
        return {};
    }
};
}
REGISTER_MODULE(ParallelStopModule, "parallelStopTest")

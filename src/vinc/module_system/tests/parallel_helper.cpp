#include <vinc/module/base_module.hpp>
#include <vinc_messages/Frame.hpp>
#include <array>
#include <atomic>
#include <condition_variable>
#include <optional>
#include <stdexcept>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto overlap_timeout = 3s;
constexpr std::uint64_t sample_count = 4;
constexpr std::int32_t completed_reply = 42;

void check(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
template<class T>
T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

class ParallelModule final : public BaseModule<RunMode::FrameworkLoop> {
    std::thread::id control_thread_;
    std::array<std::optional<message_bus::Publisher<Frame>>, 2> publishers_;
    std::array<std::optional<message_bus::Subscriber<Frame>>, 3> subscribers_;
    std::array<std::atomic<int>, 2> active_{};
    std::array<std::uint64_t, 3> last_{};
    std::array<std::unique_ptr<Frame>, 3> retained_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint32_t first_topics_ = 0;
    std::atomic<std::uint64_t> delivered_{0};
    rpc::RpcClient<Frame, Frame> rpc_client_;
    std::atomic<int> control_active_{0};
    bool param_entered_ = false;
    bool param_written_ = false;
    bool param_checked_ = false;
    bool shell_seen_ = false;
    std::uint32_t replies_ = 0;

    // Every control hook uses the same caller thread, even while a Topic waits.
    struct ControlScope {
        ParallelModule& module;
        explicit ControlScope(ParallelModule& owner) : module(owner) {
            check(std::this_thread::get_id() == module.control_thread_, "control thread changed");
            check(module.control_active_.fetch_add(1) == 0, "control callbacks overlapped");
            module.check_rpc_guard();
        }
        ~ControlScope() {
            --module.control_active_;
        }
    };

    void check_rpc_guard() {
        auto messages = make_rpc_messages<Frame, Frame>();
        const auto before = context.rpc_stats().pending;
        auto call = context.call_rpc("other.echo", messages);
        check(!call && call.error().is(rpc::ErrorCode::BlockingCallInPump), "callback RPC guard");
        check(context.rpc_stats().pending == before, "rejected call reserved a slot");
    }

    void verify_control_overlap() {
        take(context.param.set("value", std::int32_t{1}));
        {
            std::unique_lock lock(mutex_);
            check(changed_.wait_for(lock, overlap_timeout, [&] { return param_entered_; }),
                  "Param did not overlap Topic");
        }
        std::int32_t copied = -1;
        take(context.param.update("value", copied));
        check(copied == 1, "concurrent cached read");
        const auto current = take(context.param.read_current("value"));
        take(context.param.set("value", std::int32_t{2}));
        const auto newer = take(context.param.read_current("value"));
        check(newer.version > current.version && std::get<std::int32_t>(current.value) == 1,
              "independent current snapshot or revision changed");
        {
            std::lock_guard lock(mutex_);
            param_written_ = true;
        }
        changed_.notify_all();
        auto request = make_message<Frame>();
        request->data().sequence = 17;
        take(rpc_client_.call(*request));
        std::unique_lock lock(mutex_);
        check(changed_.wait_for(lock, overlap_timeout, [&] {
            return param_checked_ && shell_seen_ && replies_ == 2;
        }), "serialized control work failed to progress beside Topic");
    }

    void receive(std::size_t subscription, std::unique_ptr<Frame> message) {
        const std::size_t topic = subscription == 1 ? 1 : 0;
        check(std::this_thread::get_id() != control_thread_, "Topic callback must run on worker");
        check(active_[topic].fetch_add(1) == 0, "same Topic callbacks overlapped");
        const auto sequence = message->data().sequence.get();
        check(sequence == last_[subscription] + 1, "cursor sequence changed");
        last_[subscription] = sequence;

        // The first two distinct Topics must overlap. No sleep is used as proof.
        if (subscription < 2 && sequence == 1) {
            std::unique_lock lock(mutex_);
            ++first_topics_;
            changed_.notify_all();
            check(changed_.wait_for(lock, overlap_timeout, [&] { return first_topics_ == 2; }),
                  "different Topics failed to overlap");
        }
        if (subscription == 0 && sequence == 1) {
            verify_control_overlap();
        }
        auto recursive = context.pumpOnce();
        check(!recursive && recursive.error().is(module_system::ErrorCode::Busy), "worker recursive pump");
        auto waiting = context.waitForWork();
        check(!waiting && waiting.error().is(module_system::ErrorCode::Busy), "worker recursive wait");
        auto messages = make_rpc_messages<Frame, Frame>();
        auto call = context.call_rpc("other.echo", messages);
        check(!call && call.error().is(rpc::ErrorCode::BlockingCallInPump), "worker synchronous RPC guard");

        if (sequence == 1) {
            retained_[subscription] = std::move(message);
        } else {
            check(retained_[subscription]->data().sequence.get() == 1, "retained copy changed");
        }
        active_[topic].fetch_sub(1);
        ++delivered_;
    }
public:
    ParallelModule() : BaseModule("parallelNode", {.topic_workers = 2}) {
    }
    Result<void> on_start(const std::vector<std::string>&) override {
        control_thread_ = std::this_thread::get_id();
        take(context.param.register_param("value", std::int32_t{0}));
        take(context.param.on_change([this](const param::ParamMap& values) {
            ControlScope scope{*this};
            const auto& entry = values.at("parallelNode.value");
            if (std::get<std::int32_t>(entry.value) != 1) {
                return;
            }
            check(active_[0] == 1, "Param must overlap Topic");
            const auto version = entry.version;
            std::unique_lock lock(mutex_);
            param_entered_ = true;
            changed_.notify_all();
            check(changed_.wait_for(lock, overlap_timeout, [&] { return param_written_; }), "Topic write timed out");
            check(entry.version == version && std::get<std::int32_t>(entry.value) == 1,
                  "borrowed Param map mutated during callback");
            param_checked_ = true;
            changed_.notify_all();
        }));
        take(context.register_rpc<Frame, Frame>("echo", [this](const Frame& request, Frame& response) {
            ControlScope scope{*this};
            check(active_[0] == 1, "RPC service must overlap Topic");
            response.data().sequence = request.data().sequence.get();
            return rpc::RpcStatus::success();
        }));
        rpc_client_ = take(context.rpc_client<Frame, Frame>("parallelNode.echo", [this](auto reply) {
            ControlScope scope{*this};
            auto value = take(std::move(reply));
            check(value.response->data().sequence.get() == 17, "async RPC response");
            bool again;
            {
                std::lock_guard lock(mutex_);
                again = ++replies_ == 1;
            }
            if (again) {
                take(rpc_client_.call(*value.response));
            }
            changed_.notify_all();
        }));
        publishers_[0].emplace(take(context.advertise<Frame>("parallel/a", {.slots = 8, .max_message_bytes = 8192})));
        publishers_[1].emplace(take(context.advertise<Frame>("parallel/b", {.slots = 8, .max_message_bytes = 8192})));
        for (std::size_t index = 0; index < subscribers_.size(); ++index) {
            const auto topic = index == 1 ? "parallel/b" : "parallel/a";
            subscribers_[index].emplace(take(context.subscribe<Frame>(topic,
                [this, index](std::unique_ptr<Frame> message) { receive(index, std::move(message)); })));
        }
        for (std::uint64_t sequence = 1; sequence <= sample_count; ++sequence) {
            for (auto& publisher : publishers_) {
                auto message = make_message<Frame>();
                message->data().sequence = sequence;
                auto pushed = publisher->push(*message);
                if (!pushed) {
                    return std::unexpected(pushed.error());
                }
            }
        }
        return {};
    }
    Result<std::int32_t> module_interface(std::vector<std::string>) override {
        ControlScope scope{*this};
        if (active_[0] == 1) {
            std::lock_guard lock(mutex_);
            shell_seen_ = true;
            changed_.notify_all();
        }
        if (delivered_ == sample_count * subscribers_.size()) {
            return completed_reply;
        }
        return 0;
    }
    Result<void> on_stop() override {
        check(active_[0] == 0 && active_[1] == 0, "FrameworkLoop workers still running");
        check(delivered_ == sample_count * subscribers_.size(), "missing callbacks");
        return {};
    }
};
}
REGISTER_MODULE(ParallelModule, "parallelTest")

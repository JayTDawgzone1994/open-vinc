#pragma once
#include <vinc/param/detail/storage.hpp>
#include <vinc/param/detail/persistence.hpp>
#include <vinc/control/channel.hpp>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <set>
#include <thread>

namespace vinc::param::detail {
class ParamManager {
public:
    static Result<std::unique_ptr<ParamManager>> create(std::unique_ptr<Storage> storage);
    ~ParamManager();
    void admit(std::uint64_t mid, std::string name);
    void exited(std::uint64_t mid);
    Result<void> submit(std::uint64_t mid, std::shared_ptr<control::Session> session, control::Frame frame);
    Result<void> configure(std::filesystem::path path);
    Result<void> register_framework(std::string name, Value defaults);
    Result<std::string> command(std::vector<std::string> args, std::stop_token stop = {});
    Result<Snapshot> inspect();
    std::string diagnostic(std::uint64_t& cursor);

private:
    std::unique_ptr<Storage> storage_;
    Persistence persistence_;
    SavedMap saved_;
    Snapshot live_;
    std::map<std::string, std::uint64_t> registered_;

    struct Peer {
        std::string name;
        std::weak_ptr<control::Session> session;
        std::set<std::string> watched;
        std::uint64_t last_request = 0;
    };

    // The brief queue/identity mutex never covers shared-lock waits, disk I/O or send.
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> queue_;
    std::map<std::uint64_t, Peer> peers_;
    bool closing_ = false;
    std::jthread worker_;
    std::optional<Error> file_failure_;
    std::string notice_;
    std::mutex diagnostic_mutex_;
    std::uint64_t diagnostic_revision_ = 0;
    std::optional<Error> diagnostic_error_;
    std::string diagnostic_message_;
    void report(std::optional<Error> error, std::string message = {});
    void run(std::stop_token stop);
    Result<void> enqueue(std::function<void()> work);
    Result<control::Bytes> execute(std::uint64_t mid, const control::Frame& request);
    Result<void> register_value(std::uint64_t mid, std::string name, Value defaults, Permission permission);
    Result<void> set_value(std::uint64_t mid, const std::string& name, Value value);
    Result<void> commit(Snapshot next, const std::string& name);
    Result<void> save();
    void persist_defaults();
    Result<void> load(std::filesystem::path path);
    Result<std::string> execute_command(const std::vector<std::string>& args);

    template <class F>
    auto call(F action, bool mutation = false, std::stop_token stop = {}) -> decltype(action()) {
        using Return = decltype(action());
        auto promise = std::make_shared<std::promise<Return>>();
        auto future = promise->get_future();
        auto queued = enqueue([promise, action = std::move(action)]() mutable {
            try {
                promise->set_value(action());
            } catch (const std::bad_alloc&) {
                promise->set_value(
                    std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Create}));
            } catch (const std::exception&) {
                promise->set_value(std::unexpected(Error{ErrorCode::FileFailed, ErrorOperation::Save}));
            }
        });
        if (!queued) {
            return std::unexpected(queued.error());
        }

        const auto deadline = std::chrono::steady_clock::now() + config::param_request_timeout;
        while (future.wait_for(config::param_wait_poll_interval) != std::future_status::ready) {
            if (stop.stop_requested()) {
                const auto code = mutation ? ErrorCode::CancelledOutcomeUnknown : ErrorCode::Cancelled;
                return std::unexpected(Error{code, ErrorOperation::Receive});
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                // A timed-out request is not replayed or claimed to have been cancelled.
                const auto code = mutation ? ErrorCode::TimeoutOutcomeUnknown : ErrorCode::Timeout;
                return std::unexpected(Error{code, ErrorOperation::Receive});
            }
        }
        return future.get();
    }
};
}

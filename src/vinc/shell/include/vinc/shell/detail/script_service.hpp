#pragma once
#include <vinc/shell/detail/tcl_runtime.hpp>
#include <map>
#include <mutex>

namespace vinc::shell {
// Shared by all Shell sessions. Owner 1 is the local terminal; remote IDs are never reused.
// Control calls are thread-safe. Only framework-owned copies cross the mailbox boundary.
class ScriptService {
public:
    struct Delivery {
        std::string output;
        std::optional<ScriptReport> result;
        bool truncated = false;
    };

    explicit ScriptService(ModuleManager& manager) : router_(manager) {
    }

    Result<void> initialize() {
        return router_.initialize();
    }

    Result<void> start(std::uint64_t owner, std::filesystem::path path, std::vector<std::string> arguments);
    bool cancel(std::uint64_t owner = 0); // 0 means the global job, otherwise match its owner.
    bool active(std::uint64_t owner = 0);
    Delivery take(std::uint64_t owner);
    void forget(std::uint64_t owner);
    void shutdown();

    bool shutdown_requested() const noexcept {
        return shutdown_requested_.load();
    }

private:
    void poll(); // mutex held; finish only a ready future.
    std::mutex mutex_;
    CommandRouter router_;
    std::uint64_t owner_ = 0;
    bool stopping_ = false;
    std::map<std::uint64_t, Delivery> mail_;
    std::atomic<bool> shutdown_requested_{false};
    TclRuntime runtime_; // Join before destroying the router used by its worker.
};
}

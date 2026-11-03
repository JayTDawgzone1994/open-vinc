#pragma once

#include "../../../../tests/support/manager.hpp"
#include <vinc/module/host/module_manager.hpp>
#include <fstream>
#include <iostream>
#include <thread>

namespace vinc::test::plugin {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
inline constexpr auto wait_budget = 8s;
inline constexpr auto cleanup_budget = 12s;
inline constexpr auto poll_interval = 10ms;
inline int cleanup_failures = 0;

inline void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

template<class T>
T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}

struct CleanupEvidence {
    bool shutdown_confirmed = false;
    bool files_removed = false;
    std::filesystem::path marker;
    std::string marker_after_shutdown;
    std::vector<ModuleSnapshot> snapshots;
};

// Own the Manager and its files together. Unwinding must reap before unlinking
// a mapped Plugin, including when an assertion fails after a child starts.
class Scope {
public:
    using Shutdown = Result<void> (*)(ModuleManager&, Deadline);
    static Result<void> shutdown(ModuleManager& manager, Deadline deadline) {
        return manager.wait_shutdown(deadline);
    }

    explicit Scope(CleanupEvidence* evidence = nullptr, Shutdown stop = shutdown)
        : evidence_(evidence), shutdown_(stop) {
        constexpr int reservation_attempts = 16;
        for (int attempt = 0; attempt < reservation_attempts; ++attempt) {
            directory = std::filesystem::temp_directory_path() / ("vinc-plugin-" + instance_name());
            if (std::filesystem::create_directory(directory)) {
                return;
            }
        }
        throw std::runtime_error("cannot reserve Plugin test directory");
    }

    ~Scope() {
        (void)finish();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    void create(std::filesystem::path host = host_path()) {
        check(!manager, "one Manager per Plugin test scope");
        manager = take(create_manager({}, std::move(host)));
    }

    bool finish() noexcept {
        if (finished_) {
            return succeeded_;
        }
        finished_ = true;
        try {
            if (manager) {
                auto stopped = shutdown_(*manager, Clock::now() + cleanup_budget);
                if (!stopped) {
                    std::cerr << "Plugin test shutdown failed: " << error_to_string(stopped.error()) << '\n';
                    return retain_files();
                }
                if (evidence_) {
                    evidence_->snapshots = manager->snapshots();
                    if (!evidence_->marker.empty()) {
                        std::ifstream marker(evidence_->marker);
                        std::getline(marker, evidence_->marker_after_shutdown);
                    }
                }
            }
            if (evidence_) {
                evidence_->shutdown_confirmed = true;
            }
            manager.reset();
            std::filesystem::remove_all(directory);
            if (evidence_) {
                evidence_->files_removed = true;
            }
            succeeded_ = true;
        } catch (const std::exception& error) {
            std::cerr << "Plugin test cleanup exception: " << error.what() << '\n';
            return retain_files();
        } catch (...) {
            return retain_files();
        }
        return succeeded_;
    }

    std::filesystem::path directory;
    std::unique_ptr<ModuleManager> manager;

private:
    bool retain_files() noexcept {
        ++cleanup_failures;
        // Manager destruction still attempts termination. No confirmed shutdown
        // means files remain for diagnosis even if that final attempt succeeds.
        std::cerr << "Plugin test files retained: " << directory << '\n';
        manager.reset();
        return false;
    }

    CleanupEvidence* evidence_ = nullptr;
    Shutdown shutdown_;
    bool finished_ = false;
    bool succeeded_ = false;
};

inline void signal_file(const std::filesystem::path& path) {
    std::ofstream output(path);
    output << "ready\n";
    output.close();
    check(bool(output), "write test gate");
}

inline void wait_file(const std::filesystem::path& path, Deadline deadline) {
    while (!std::filesystem::exists(path)) {
        check(Clock::now() < deadline, "test gate deadline expired");
        std::this_thread::sleep_for(poll_interval);
    }
}
} // namespace vinc::test::plugin

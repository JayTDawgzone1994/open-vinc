#include "../../../../tests/support/manager.hpp"
#include "../../param/tests/support.hpp"
#include <vinc/osal/detail/native_error.hpp>
#include <sys/resource.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {
using namespace vinc;
using namespace param_test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr auto startup_timeout = 10s;
constexpr auto command_timeout = 5s;
constexpr auto shutdown_timeout = 10s;
constexpr int rejected_attempts = 3;
constexpr int canary_sequence = 42;
constexpr char canary_topic[] = "registration/canary";

// Apply pressure only after the Hosts and their control sessions are running.
// A zero soft limit leaves existing descriptors usable, regardless of which fds CTest inherited.
class NoNewDescriptors {
    rlimit previous_{};

public:
    NoNewDescriptors() {
        check(::getrlimit(RLIMIT_NOFILE, &previous_) == 0, "read soft fd limit");
        auto exhausted = previous_;
        exhausted.rlim_cur = 0;
        check(::setrlimit(RLIMIT_NOFILE, &exhausted) == 0, "exhaust central fd table");
    }

    NoNewDescriptors(const NoNewDescriptors&) = delete;
    NoNewDescriptors& operator=(const NoNewDescriptors&) = delete;

    ~NoNewDescriptors() {
        if (::setrlimit(RLIMIT_NOFILE, &previous_) != 0) {
            std::perror("restore central fd limit");
        }
    }
};

std::size_t reservation_count(const fs::path& journal) {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(journal)) {
        if (entry.is_directory()) {
            ++count;
        }
    }
    return count;
}

void require_create_emfile(const Error& error) {
    const auto causes = error.causes();
    check(!causes.empty(), "missing native failure in subscribe error");
    const auto& deepest = causes.back();
    const auto expected = Error{osal::ErrorCode::ResourceExhausted, osal::ErrorOperation::CreateSharedMemory,
                                {osal::detail::errno_source, EMFILE}};
    check(deepest == expected.record(), "expected OSAL CreateSharedMemory with native EMFILE");
}

void run(const fs::path& plugin) {
    Directory home;
    check(::setenv("HOME", home.path.c_str(), 1) == 0, "isolate central instance journal");
    auto manager = take(test::create_manager());
    ModulePolicy policy;
    policy.max_restarts = 0;
    take(manager->register_module_type({"messageBusTest", plugin, policy}));

    auto start = [&](std::vector<std::string> args) {
        const auto mid = take(manager->start_module("messageBusTest", std::move(args)));
        take(manager->wait_running(mid, control::Clock::now() + startup_timeout));
        return mid;
    };
    const auto writer = start({"--name", "writer", "pub", canary_topic, "2", "4096"});
    const auto reader = start({"--name", "reader", "sub", canary_topic});
    auto command = [&](MID mid, std::vector<std::string> args) {
        return manager->command(mid, std::move(args), control::Clock::now() + command_timeout);
    };

    const auto epochs = home.path / ".open-vinc-state-v1" / "epochs";
    auto epoch = fs::directory_iterator(epochs);
    check(epoch != fs::directory_iterator{}, "missing test instance journal");
    const auto journal = epoch->path();
    ++epoch;
    check(epoch == fs::directory_iterator{}, "expected one isolated central instance");
    const auto baseline = reservation_count(journal);
    check(baseline > 0, "expected active Topic and Module reservations before applying fd pressure");
    bool clean = true;
    for (int attempt = 1; attempt <= rejected_attempts; ++attempt) {
        const auto topic = "registration/retry" + std::to_string(attempt);
        const auto before = reservation_count(journal);
        {
            NoNewDescriptors exhausted;
            auto refused = command(reader, {"probe-subscription", topic});
            check(!refused, "new Topic registered under fd exhaustion");
            std::cout << topic << " " << error_to_string(refused.error()) << '\n';
            require_create_emfile(refused.error());

            const int sequence = canary_sequence + attempt;
            check(take(command(writer, {"push", std::to_string(sequence)})) == 0, "existing publisher failed");
            check(take(command(reader, {"last"})) == sequence, "existing subscriber did not read new sequence");
        }
        // Counting opens a directory; inspect after restoring the limit and before registering the next Topic.
        const auto after = reservation_count(journal);
        std::cout << topic << " journal before=" << before << " after=" << after << '\n';
        if (after != before) {
            clean = false;
        }
    }

    check(take(command(reader, {"probe-subscription", "registration/recovered"})) == 0, "retry after fd recovery");
    const auto final_count = reservation_count(journal);
    std::cout << "live central journal baseline=" << baseline << " final=" << final_count << '\n';
    take(manager->wait_shutdown(control::Clock::now() + shutdown_timeout));
    manager.reset();
    const auto remaining = reservation_count(journal);
    std::cout << "after shutdown reservations=" << remaining << '\n';
    check(clean && final_count == baseline && remaining == 0, "failed Topic registration left journal reservations");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected path to test Module");
        run(fs::absolute(argv[1]));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

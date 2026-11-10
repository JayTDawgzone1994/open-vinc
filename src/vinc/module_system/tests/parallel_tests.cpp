#include "../../../../tests/support/manager.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto test_timeout = 12s;
constexpr auto command_timeout = 2s;
constexpr auto observation_interval = 10ms;
constexpr std::int32_t completed_reply = 42;

template<class T>
auto take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("parallel Plugin argument required");
        }
        auto manager = take(vinc::test::create_manager());
        ModuleType type{"parallelTest", argv[1], {}};
        type.policy.max_restarts = 0;
        take(manager->register_module_type(type));
        auto mid = take(manager->start_module(type.name));
        take(manager->wait_running(mid, control::Clock::now() + test_timeout));
        bool completed = false;
        const auto deadline = control::Clock::now() + test_timeout;
        while (control::Clock::now() < deadline) {
            auto reply = take(manager->command(mid, {}, control::Clock::now() + command_timeout));
            if (reply == completed_reply) {
                completed = true;
                break;
            }
            std::this_thread::sleep_for(observation_interval);
        }
        if (!completed) {
            throw std::runtime_error("parallel callback completion timed out");
        }
        take(manager->stop_module("parallelNode", control::Clock::now() + test_timeout));
        for (const auto& snapshot : manager->snapshots()) {
            if (snapshot.state != ModuleState::Stopped || snapshot.last_failure || snapshot.cleanup_failure) {
                throw std::runtime_error("parallel Module failed during shutdown");
            }
        }
        manager->begin_shutdown();
        take(manager->wait_shutdown(control::Clock::now() + test_timeout));
        std::cout << "Parallel Context through real Plugin passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

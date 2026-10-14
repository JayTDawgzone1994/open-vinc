#include "../../../../tests/support/manager.hpp"
#include <vinc/module/host/module_manager.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;

namespace {
constexpr auto deadline_limit = 20s;
constexpr auto poll = 10ms;

template <class T> auto take(Result<T> result) {
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
        if (argc != 2 && argc != 3) {
            return 2;
        }

        auto manager = take(test::create_manager());
        auto output = take(manager->output_hub().subscribe());
        take(manager->register_module_type({"rpcFixture", std::filesystem::absolute(argv[1]), {}}));
        auto server = take(manager->start_module("rpcFixture", {"server"}));
        take(manager->wait_started(server));
        const bool target_exit = argc == 3;
        auto caller = take(manager->start_module("rpcFixture", {target_exit ? argv[2] : "caller"}));
        take(manager->wait_started(caller));
        const auto deadline = std::chrono::steady_clock::now() + deadline_limit;
        bool passed = false;
        while (std::chrono::steady_clock::now() < deadline) {
            auto snapshot = manager->inspect(caller);
            if (snapshot && snapshot->last_failure) {
                std::cerr << error_to_string(snapshot->last_failure->error) << '\n';
            }

            if (snapshot && snapshot->state == ModuleState::Stopped) {
                passed = true;
                break;
            }

            if (snapshot && snapshot->state == ModuleState::Failed) {
                break;
            }

            std::this_thread::sleep_for(poll);
        }

        for (auto& event : output.take().events) {
            std::cout << event->bytes;
        }

        const auto expected = target_exit ? ModuleState::Failed : ModuleState::Running;
        if (!passed || manager->inspect(server)->state != expected) {
            return 1;
        }

        manager->begin_shutdown();
        take(manager->wait_shutdown(std::chrono::steady_clock::now() + deadline_limit));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

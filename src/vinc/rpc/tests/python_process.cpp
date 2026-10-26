#include "../../../../tests/support/manager.hpp"
#include <vinc/module/host/python_modules.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;

namespace {
constexpr auto deadline_limit = 15s;
constexpr auto command_timeout = 2s;
constexpr auto poll = 20ms;

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
        if (argc != 5) {
            return 2;
        }

        auto manager = take(test::create_manager());
        auto output = take(manager->output_hub().subscribe());
        take(manager->register_module_type({"rpcNativeFixture", std::filesystem::absolute(argv[4]), {}}));
        auto native = take(manager->start_module("rpcNativeFixture"));
        take(manager->wait_started(native));
        ModuleType type;
        type.name = "rpcPythonFixture";
        type.python = PythonLaunch{std::filesystem::absolute(argv[3]), std::filesystem::absolute(argv[2]),
                                   std::filesystem::absolute(argv[1])};
        take(manager->register_module_type(type));
        auto python = take(manager->start_module(type.name, {"--name", "rpcPython"}));
        take(manager->wait_started(python));
        const auto deadline = control::Clock::now() + deadline_limit;
        bool done = false;
        while (control::Clock::now() < deadline) {
            const auto native_ready =
                take(manager->command(native, {}, control::Clock::now() + command_timeout));
            const auto python_ready =
                take(manager->command(python, {}, control::Clock::now() + command_timeout));
            if (native_ready == 1 && python_ready == 1) {
                done = true;
                break;
            }

            std::this_thread::sleep_for(poll);
        }

        for (const auto& event : output.take().events) {
            std::cout << event->bytes;
        }

        manager->begin_shutdown();
        take(manager->wait_shutdown(control::Clock::now() + deadline_limit));
        return done ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

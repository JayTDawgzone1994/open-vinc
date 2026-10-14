#include "../../../../tests/support/manager.hpp"
#include <vinc/osal/path.hpp>
#include <httplib.h>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
constexpr auto operation_budget = std::chrono::seconds{15};
constexpr auto observe_interval = std::chrono::milliseconds{20};

void check(bool condition, const char* text) {
    if (!condition) {
        throw std::runtime_error(text);
    }
}

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

void take(vinc::Result<void> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "plugin and assets arguments");
        auto manager = take(vinc::test::create_manager());
        vinc::ModuleType type{"webUi", std::filesystem::absolute(argv[1]), {}};
        type.policy.stop_timeout = vinc::config::ui_stop_timeout;
        take(manager->register_module_type(type));
        check(manager->get_active_module_names().empty(), "registered UI does not auto-start");
        auto output = take(manager->output_hub().subscribe());
        const auto root = take(vinc::osal::path_to_utf8(std::filesystem::absolute(argv[2])));
        const auto mid = take(manager->start_module("webUi", {"--root", root, "--port", "0"}));
        take(manager->wait_started(mid));
        std::string trace;
        const auto deadline = std::chrono::steady_clock::now() + operation_budget;
        const std::string prefix = "http://127.0.0.1:";
        while (trace.find(prefix) == std::string::npos) {
            for (const auto& event : output.take().events) {
                trace += event->bytes;
            }

            check(std::chrono::steady_clock::now() < deadline, "UI startup output observed");
            std::this_thread::sleep_for(observe_interval);
        }

        const auto port = std::stoi(trace.substr(trace.find(prefix) + prefix.size()));
        httplib::Client client("127.0.0.1", port);
        client.set_read_timeout(operation_budget);
        const auto response = client.Get("/api/ui");
        check(response && response->status == 200, "ModuleHost serves HTTP");
        check(take(manager->command(mid, {"status"}, deadline)) == 0, "Shell hook works through pump");
        take(manager->stop_module(mid, deadline));
        auto stopped = manager->inspect(mid);
        check(stopped && stopped->state == vinc::ModuleState::Stopped && !stopped->last_failure,
              "UI stopped through normal Module lifecycle");
        const auto restarted = take(manager->start_module("webUi", {"--root", root, "--port", std::to_string(port)}));
        take(manager->wait_started(restarted));
        take(manager->stop_module(restarted, std::chrono::steady_clock::now() + operation_budget));
        const auto failed = take(manager->start_module("webUi", {"--root", root, "--port", "invalid"}));
        check(!manager->wait_started(failed), "bad option is a Module startup error");
        manager->begin_shutdown();
        take(manager->wait_shutdown(std::chrono::steady_clock::now() + operation_budget));
        std::cout << "UI module tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

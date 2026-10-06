#pragma once
#include <vinc/osal/process.hpp>
#include <vinc/detail/runtime_services.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace contract_test {
inline std::filesystem::path executable;
inline unsigned selected = 0;
inline unsigned current = 0;
inline constexpr int violation_exit = vinc::detail::contract_fallback_exit_status;
inline constexpr int unexpected_terminate_exit = 78;
inline constexpr auto probe_timeout = std::chrono::seconds(15);

inline bool initialize(int argc, char** argv) {
    executable = std::filesystem::absolute(argv[0]);
    if (argc == 3 && std::string_view(argv[1]) == "--contract-probe") {
        selected = static_cast<unsigned>(std::stoul(argv[2]));
        // Contract fallback bypasses terminate handlers. Unrelated C++ termination
        // must not satisfy the parent's expectation for a contract violation.
        std::set_terminate([] {
            std::_Exit(unexpected_terminate_exit);
        });
        return true;
    }

    return false;
}

template <class F> void expect(F&& action) {
    ++current;
    if (selected != 0) {
        if (current == selected) {
            action();
            std::_Exit(0);
        }
        return;
    }
    auto service = vinc::osal::ProcessService::create();
    if (!service) {
        throw std::runtime_error("contract probe service failed");
    }
    vinc::osal::SpawnOptions options;
    options.executable = executable;
    options.arguments = {"--contract-probe", std::to_string(current)};
    auto child = service->spawn(options);
    if (!child) {
        throw std::runtime_error("contract probe spawn failed");
    }
    auto status = child->wait_until(std::chrono::steady_clock::now() + probe_timeout);
    if (!status) {
        (void)child->terminate();
        (void)child->wait_until(std::chrono::steady_clock::now() + probe_timeout);
        throw std::runtime_error("contract probe timed out");
    }
    (void)service->forget(child->id());
    if (status->kind != vinc::osal::ExitKind::Code || status->value != violation_exit) {
        throw std::runtime_error("contract probe did not terminate through the contract handler");
    }
}
}

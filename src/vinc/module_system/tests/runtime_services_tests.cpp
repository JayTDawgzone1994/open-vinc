#include <vinc/module/detail/contract_report.hpp>
#include <vinc/osal/process.hpp>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>

namespace {
using namespace vinc;
constexpr MID fixture_mid = 456;
constexpr auto process_timeout = std::chrono::seconds(10);

template<class T>
T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
    return std::move(*result);
}

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void run(const std::filesystem::path& helper, const char* mode, const char* expected_reason) {
    const auto name = "bridge_" + std::to_string(std::random_device{}());
    auto storage = take(detail::ContractReportStorage::create(name, fixture_mid));
    auto processes = take(osal::ProcessService::create());
    osal::SpawnOptions options;
    options.executable = helper;
    options.arguments = {name, mode};
    auto child = take(processes.spawn(options));
    auto exit = child.wait_until(std::chrono::steady_clock::now() + process_timeout);
    if (!exit) {
        (void)child.terminate();
        (void)child.wait_until(std::chrono::steady_clock::now() + process_timeout);
        (void)processes.forget(child.id());
        throw std::runtime_error("bridge child did not exit");
    }

    check(bool(processes.forget(child.id())), "forget exited bridge child");
    auto expected_exit = detail::contract_fallback_exit_status;
    if (expected_reason) {
        expected_exit = detail::contract_exit_status;
    } else if (std::string_view(mode) == "validation") {
        expected_exit = 0;
    }
    check(exit->kind == osal::ExitKind::Code && exit->value == static_cast<std::uint32_t>(expected_exit),
          "bridge exit status");

    const auto report = take(storage->read_after_exit());
    if (expected_reason) {
        check(report.has_value(), "Host's single record contains the DLL report");
        check(std::string_view(report->reason.data()) == expected_reason, "expected image called Host callback");
        check(report->has_index && report->index == 23 && report->size == 7, "bounds survived bridge");
        check(report->line > 0 && report->function[0] != '\0', "source location survived bridge");
        const std::string_view file = report->file.data();
        const auto source = std::string_view(expected_reason) == "host"
                                ? "runtime_services_host.cpp"
                                : "runtime_services_plugin.cpp";
        check(file.find(source) != std::string_view::npos, "location belongs to originating image");
    } else {
        check(!report, "unbound/withdrawn Plugin must not discover another image's binding");
    }
    check(bool(storage->close()), "release bridge record");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "bridge Host fixture path required");
        const auto helper = std::filesystem::absolute(argv[1]);
        run(helper, "validation", nullptr);
        run(helper, "plugin-a", "plugin-a");
        run(helper, "plugin-b", "plugin-b");
        run(helper, "clear-a-then-b", "plugin-b");
        run(helper, "host", "host");
        run(helper, "unbound", nullptr);
        run(helper, "second-unbound", nullptr);
        run(helper, "cleared", nullptr);
        run(helper, "teardown", nullptr);
        std::cout << "runtime services DLL bridge tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

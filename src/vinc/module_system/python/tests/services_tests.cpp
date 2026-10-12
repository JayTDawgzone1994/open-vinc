#include "../../../../../tests/support/manager.hpp"
#include <vinc/module/host/python_modules.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto finish_timeout = 15s;
constexpr auto poll_interval = 10ms;

void check(bool condition, const char* text) {
    if (!condition) {
        throw std::runtime_error(text);
    }
}
template<class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

void run(const std::filesystem::path& python, const std::filesystem::path& bootstrap,
         const std::filesystem::path& script, const std::filesystem::path& plugin, std::string mode) {
    auto manager = take(test::create_manager());
    auto output = take(manager->output_hub().subscribe());
    const auto root = std::filesystem::temp_directory_path() / test::instance_name();
    take(detail::ManagerAccess::configure_params(*manager, root));
    take(detail::ManagerAccess::configure_log(*manager, root));
    ModuleType native{"nativePeer", plugin, "nativeNode", {}};
    take(manager->register_module_type(native));
    auto native_mid = take(manager->start_module(native.name));
    take(manager->wait_started(native_mid));
    ModuleType type;
    type.name = "pythonServices";
    type.module_base_name = "pythonNode";
    type.python = PythonLaunch{script, bootstrap, python};
    take(manager->register_module_type(type));
    auto mid = take(manager->start_module(type.name, {"--name", "pythonNode", mode}));
    take(manager->wait_started(mid));
    const auto deadline = control::Clock::now() + finish_timeout;
    std::optional<ModuleSnapshot> record;
    while (control::Clock::now() < deadline) {
        record = manager->inspect(mid);
        if (record && (record->state == ModuleState::Stopped || record->state == ModuleState::Failed)) {
            break;
        }

        std::this_thread::sleep_for(poll_interval);
    }
    std::string captured;
    for (const auto& event : output.take().events) {
        captured += event->bytes;
    }
    std::cout << captured;
    if (record && record->last_failure) {
        std::cout << error_to_string(record->last_failure->error) << '\n';
    }
    if (mode == "normal") {
        check(record && record->state == ModuleState::Stopped, "Python services must finish successfully");
        check(captured.find("PYTHON_SERVICES_PASSED") != std::string::npos, "all Python service checks ran");
        check(captured.find("PYTHON_SERVICE_REFERENCES_RELEASED") != std::string::npos,
              "callback cycles released after Context revocation");
        log::View view;
        view.enabled = true;
        auto text = detail::ManagerAccess::log_take(*manager, view);
        check(text.find("PYTHON_STRUCTURED_LOG") != std::string::npos, "Python structured log reaches central");
        check(text.find("pythonNode") != std::string::npos, "central assigns log identity");
    } else {
        check(record && record->state == ModuleState::Failed && record->last_failure,
              "uncaught callback exception fails its module");
        auto operation = mode == "param-error" ? module_system::ErrorOperation::ParamCallback
                                               : module_system::ErrorOperation::SubscriptionCallback;
        check(record->last_failure->error.is_operation(operation), "callback operation retained");
        check(captured.find("services_module.py(") != std::string::npos,
              "callback traceback retains source location");
        if (mode == "vinc-error") {
            const auto causes = record->last_failure->error.causes();
            check(!causes.empty() && causes.back().code == static_cast<std::int32_t>(module_system::ErrorCode::Busy) &&
                  causes.back().domain == ErrorDomain::ModuleSystem, "callback retains structured VINC root cause");
        }
    }
    check(manager->inspect(native_mid)->state == ModuleState::Running, "other module remains alive");
    manager->begin_shutdown();
    take(manager->wait_shutdown(control::Clock::now() + finish_timeout));
    manager.reset();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 5, "interpreter/bootstrap/script/plugin arguments");
        for (const auto* mode : {"normal", "param-error", "bus-error", "vinc-error"}) {
            run(std::filesystem::absolute(argv[1]), std::filesystem::absolute(argv[2]),
                std::filesystem::absolute(argv[3]), std::filesystem::absolute(argv[4]), mode);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

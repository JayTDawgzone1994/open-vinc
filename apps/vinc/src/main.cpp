#include <vinc/module/host/python_modules.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/module/detail/manager_access.hpp>
#include <vinc/module/detail/module_runtime.hpp>
// Framework-owned process entry: dispatch Host launches before central services.
// Central then parses -S, registers modules and enters the Shell/deployment loop.

#include <vinc/shell/detail/shell.hpp>
#include <vinc/shell/detail/tcl_runtime.hpp>
#include <module_catalog.hpp>
#include <vinc_python_config.hpp>
#include <vinc/config.hpp>
#include <iostream>
#include <charconv>
#include <chrono>
#include <limits>
#ifdef VINC_STARTUP_UI_ERRORS
#include <vinc/ui/error.hpp>
#endif
#ifdef VINC_COMPOSED_APP
#include <vinc_app_config.hpp>
#endif
#ifdef VINC_EMBEDDED_STARTUP
#include <embedded_startup.hpp>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
constexpr int success_exit_code = 0;
constexpr int startup_failure_exit_code = 1;
constexpr int invalid_arguments_exit_code = 2;

// CLI admission limits retain the existing ranges; SocketShell validates its own options too.
constexpr std::uint32_t maximum_port = (std::numeric_limits<std::uint16_t>::max)();
constexpr std::uint32_t maximum_timeout_seconds = 86400;
constexpr std::uint32_t minimum_send_timeout_seconds = 1;
constexpr std::uint32_t minimum_connections = 1;

constexpr const char* usage =
    "Usage: application [--shell-ip IP] [--shell-port PORT] [--shell-disconnect-timeout SECONDS]\n"
    "            [--shell-send-timeout SECONDS] [--shell-max-connections 1..8]\n"
    "            [--python PATH]\n"
#ifdef VINC_EMBEDDED_STARTUP
    "Startup script is embedded; -S is not accepted.\n"
#else
    "            [-S file.tcl [args...]]\n"
#endif
    "Defaults: 127.0.0.1:5500, 30-second disconnect/send timeouts, 8 connections.\n"
    "Raw TCP, no authentication or encryption. Port 0 selects an ephemeral port.\n";

struct StartupOptions {
    std::filesystem::path script;
    std::vector<std::string> script_arguments;
    vinc::shell::SocketShellOptions network;
    std::filesystem::path python;
};

// Internal CLI control flow, not a framework error API. Help exits before service creation.
enum class ParseAction {
    Start,
    ShowHelp,
    InvalidArguments
};

ParseAction parse_arguments(int argc, char** argv, StartupOptions& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view name(argv[index]);
        if (name == "--help") {
            return ParseAction::ShowHelp;
        }

#ifdef VINC_EMBEDDED_STARTUP
        if (name == "-S") {
            std::cerr << "This application has a fixed embedded startup script; -S is not accepted.\n";
            return ParseAction::InvalidArguments;
        }
#endif

        ++index;
        if (index == argc || !*argv[index]) {
            return ParseAction::InvalidArguments;
        }

        if (name == "-S") {
            auto path = vinc::osal::path_from_utf8(argv[index]);
            if (!path) {
                std::cerr << "Invalid script path: " << vinc::error_to_string(path.error()) << '\n';
                return ParseAction::InvalidArguments;
            }

            options.script = std::move(*path);
            // Everything after the script path belongs to Tcl, even tokens resembling CLI options.
            options.script_arguments.assign(argv + index + 1, argv + argc);
            break;
        }

        if (name == "--python") {
            auto path = vinc::osal::path_from_utf8(argv[index]);
            if (!path) {
                return ParseAction::InvalidArguments;
            }
            std::error_code error;
            options.python = std::filesystem::absolute(*path, error);
            if (error) {
                return ParseAction::InvalidArguments;
            }
            continue;
        }

        if (name == "--shell-ip") {
            options.network.ip = argv[index];
            continue;
        }

        const std::string_view value(argv[index]);
        std::uint32_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            return ParseAction::InvalidArguments;
        }

        if (name == "--shell-port" && number <= maximum_port) {
            options.network.port = static_cast<std::uint16_t>(number);
        } else if (name == "--shell-disconnect-timeout" && number <= maximum_timeout_seconds) {
            options.network.disconnect_timeout = std::chrono::seconds(number);
        } else if (name == "--shell-send-timeout" && number >= minimum_send_timeout_seconds &&
                   number <= maximum_timeout_seconds) {
            options.network.send_timeout = std::chrono::seconds(number);
        } else if (name == "--shell-max-connections" && number >= minimum_connections &&
                   number <= vinc::config::shell_socket_max_connections) {
            options.network.max_connections = number;
        } else {
            return ParseAction::InvalidArguments;
        }
    }

    if (options.python.empty() && !vinc::app::python_executable.empty()) {
        auto path = vinc::osal::path_from_utf8(vinc::app::python_executable);
        if (!path) {
            std::cerr << "Invalid configured Python path: " << vinc::error_to_string(path.error()) << '\n';
            return ParseAction::InvalidArguments;
        }

        options.python = std::move(*path);
    }

    return ParseAction::Start;
}

// Admit each official instance once. Manager owns retries after this initial
// readiness wait; a failed attempt must not stop central or cancel recovery.
std::string module_start_reason(const vinc::Error& error) {
#ifdef VINC_STARTUP_UI_ERRORS
    // Presentation may explain a cause; control flow still uses the outer error.
    for (const auto& cause : error.causes()) {
        const auto detail = vinc::Error::from_raw(cause.domain, cause.code, cause.operation, cause.native);
        if (detail.is(vinc::ui::ErrorCode::BindFailed)) {
            return "unable to listen on 127.0.0.1:" + std::to_string(vinc::config::ui_port);
        }
    }
#endif
    using Code = vinc::module_system::ErrorCode;
    if (error.is(Code::HookFailed)) {
        return "initialization hook failed; see diagnostic details";
    }
    if (error.is(Code::HookException)) {
        return "initialization hook raised an exception";
    }
    if (error.is(Code::PluginLoadFailed)) {
        return "unable to load the Module plugin";
    }

    return "unable to start the Module; see diagnostic details";
}

void start_official(vinc::ModuleManager& manager, const std::string& type,
                    const std::string& display_name, const std::string& ready_description,
                    vinc::shell::StartupReport& report) {
    // Legacy library-only fixtures may omit official types. Composed Apps always
    // include them; failures here remain local so later startup can continue.
    if (!manager.has_module_type(type)) {
        return;
    }

    auto started = vinc::detail::ManagerAccess::start_system_module(manager, type);
    if (!started) {
        report.entries.push_back({display_name, module_start_reason(started.error()), started.error()});
        return;
    }

    const auto identity = " (MID=" + std::to_string(*started) + ")";
    auto ready = manager.wait_started(*started);
    if (ready) {
        report.entries.push_back({display_name, ready_description + identity, {}});
        return;
    }

    report.entries.push_back({display_name, module_start_reason(ready.error()) + identity, ready.error()});
    const auto snapshot = manager.inspect(*started);
    if (snapshot && snapshot->last_failure && snapshot->last_failure->error != ready.error()) {
        report.entries.back().related_error = snapshot->last_failure->error;
    }

    // The supervisor retains ownership and applies the System Module retry
    // window. A failed initial readiness wait must not disable that recovery.
}

vinc::Result<void> run_services(StartupOptions& options, vinc::shell::StartupReport& report) {
    auto failed = [&](std::string name, std::string description, const vinc::Error& error) -> vinc::Result<void> {
        report.entries.push_back({std::move(name), std::move(description), error});
        return std::unexpected(error);
    };
    // Both roles use this executable. Query the OS instead of trusting argv[0],
    // cwd or a producer build path; only central creates Manager and Tcl.
    auto native_executable = vinc::osal::executable_path();
    if (!native_executable) {
        return failed("Application", "unable to locate the executable", native_executable.error());
    }

    auto executable_text = vinc::osal::path_to_utf8(*native_executable);
    if (!executable_text) {
        return failed("Application", "unable to convert the executable path", executable_text.error());
    }

    auto initialized = vinc::shell::initialize_tcl(executable_text->c_str());
    if (!initialized) {
        return failed("Tcl", "unable to initialize script runtime", initialized.error());
    }
    report.entries.push_back({"Tcl", "script runtime initialized", {}});
    auto directory = vinc::shell::executable_directory();
    if (!directory) {
        return failed("Application", "unable to locate runtime resources", directory.error());
    }
#ifdef VINC_COMPOSED_APP
    auto owner = vinc::detail::ManagerAccess::create(std::string(vinc::app::instance_name), {},
        *native_executable);
#else
    auto owner = vinc::ModuleManager::create({}, *native_executable);
#endif
    if (!owner) {
        return failed("Module Manager", "unable to initialize process supervision", owner.error());
    }

    report.entries.push_back({"Module Manager", "process supervision initialized", {}});
    auto& manager = **owner;
    auto logging = vinc::detail::ManagerAccess::configure_log(manager, *directory / "vinc_data");
    if (!logging) {
        report.entries.push_back({"Log", "file output unavailable", logging.error()});
    } else {
        report.diagnostics_available = true;
        report.entries.push_back({"Log", "file output configured", {}});
    }
    auto parameters = vinc::detail::ManagerAccess::configure_params(manager, *directory / "vinc_data");
    if (!parameters) {
        // Persistence failure leaves the live service available with registered defaults.
        report.entries.push_back({"Parameters", "persistence unavailable; live parameters remain available",
                                  parameters.error()});
    } else {
        report.entries.push_back({"Parameters", "persistence configured", {}});
    }
    // Shell must be destroyed before its manager. Reserve Shell/Tcl command names
    // before registering module factories, so collisions fail at registration.
    auto shell_owner = vinc::Shell::create(manager, std::cout);
    if (!shell_owner) {
        return failed("Shell", "unable to initialize command services", shell_owner.error());
    }
    auto& shell = **shell_owner;
    report.entries.push_back({"Shell", "command and script services initialized", {}});
#ifdef VINC_COMPOSED_APP
    auto registered = vinc::register_built_modules(manager, *directory);
#else
    auto registered = vinc::register_built_modules(manager);
#endif
    if (!registered) {
        return failed("Module Catalog", "unable to register ModuleTypes", registered.error());
    }
    report.entries.push_back({"Module Catalog", "built-in ModuleTypes registered", {}});

    auto discovered = vinc::discover_python_modules(manager, *directory / "data" / "module",
        *directory / "python" / "vinc" / "bootstrap.py", options.python);
    if (!discovered) {
        report.entries.push_back({"Python discovery", "unable to scan Module sources", discovered.error()});
    } else {
        for (const auto& issue : discovered->issues) {
            auto path = vinc::osal::path_to_utf8(issue.path);
            report.entries.push_back({"Python Module", "unable to register " + (path ? *path : "<invalid path>"),
                                      issue.error});
        }
    }

    const auto endpoint = shell.start_socket_shell(std::move(options.network));
    if (!endpoint) {
        return failed("Socket Shell", "unable to start listener", endpoint.error());
    }
    const auto address = std::visit([](const auto& ip) { return ip.to_string(); }, endpoint->address);
    report.entries.push_back({"Socket Shell", "listening on " + address + ':' +
        std::to_string(endpoint->port) + " (Raw TCP)", {}});

    start_official(manager, "webUi", "Web UI", "browser workspace initialized", report);
    start_official(manager, "bagRecorder", "Bag Recorder", "idle, waiting for recording commands", report);
    shell.set_startup_report(std::move(report));
    report = {};

#ifdef VINC_EMBEDDED_STARTUP
    return shell.run_source(vinc::shell::ScriptSource::embedded(std::string(vinc::app::startup_script)));
#else
    return shell.run(std::move(options.script), std::move(options.script_arguments));
#endif
}
}

int application(int argc, char** argv) {
    // Reserved internal launches never fall back to central, including unknown
    // bootstrap versions. Later tokens belong to central/script/module arguments.
    if (argc > 1 && vinc::detail::is_module_launch(argv[1])) {
        return vinc::run_module_process(std::vector<std::string>(argv + 1, argv + argc));
    }

    try {
        StartupOptions options;
        const auto action = parse_arguments(argc, argv, options);
        if (action == ParseAction::ShowHelp) {
            std::cout << usage;
            return success_exit_code;
        }

        if (action == ParseAction::InvalidArguments) {
            std::cerr << usage;
            return invalid_arguments_exit_code;
        }

        vinc::shell::StartupReport report;
        auto result = run_services(options, report);
        if (!result) {
            if (!report.entries.empty()) {
                // Before Shell takes ownership, no startup diagnostics were queued to Log.
                report.diagnostics_available = false;
                report.write(std::cerr, false, false);
            } else {
                std::cerr << "Shell failed: " << vinc::error_to_string(result.error()) << '\n';
            }

            return startup_failure_exit_code;
        }
        return success_exit_code;
    } catch (const std::exception& error) {
        std::cerr << "Shell startup failed: " << error.what() << '\n';
        return startup_failure_exit_code;
    }
}
#ifdef _WIN32
// Windows supplies UTF-16 arguments. Convert once to the UTF-8 contract used by
// the framework and Tcl, including non-ASCII script paths and arguments.
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) {
        const int length =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, nullptr, 0, nullptr, nullptr);
        if (!length) {
            return invalid_arguments_exit_code;
        }

        // A -1 source length includes the terminator in both the size query and conversion.
        std::string value(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, value.data(), length, nullptr,
                            nullptr);
        value.pop_back();
        arguments.push_back(std::move(value));
    }

    // Finish growing the owning strings before borrowing their addresses for application().
    std::vector<char*> pointers;
    for (auto& argument : arguments) {
        pointers.push_back(argument.data());
    }

    return application(argc, pointers.data());
}
#else
int main(int argc, char** argv) {
    return application(argc, argv);
}
#endif

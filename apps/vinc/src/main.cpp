#include <vinc/module/host/python_modules.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/module/detail/manager_access.hpp>
#include <vinc/module/detail/module_runtime.hpp>
// Framework-owned process entry: dispatch Host launches before central services.
// Central then parses -S, registers modules and enters the Shell/deployment loop.

#include <vinc/shell/detail/shell.hpp>
#include <vinc/shell/detail/tcl_runtime.hpp>
#include <module_catalog.hpp>
#include <vinc/config.hpp>
#include <iostream>
#include <charconv>
#include <chrono>
#include <limits>
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

    return ParseAction::Start;
}

// Each official Module is attempted once. Ready waits retain Manager's existing
// startup deadline; failure remains local, and explicit stops are never undone.
void start_official(vinc::ModuleManager& manager, const std::string& type,
                    std::chrono::milliseconds stop_timeout) {
    // Legacy library-only fixtures may omit official types. Composed Apps always
    // include them; failures here remain local so later startup can continue.
    if (!manager.has_module_type(type)) {
        return;
    }

    auto started = manager.start_module(type);
    if (!started) {
        std::cerr << "Official Module " << type << " failed: "
                  << vinc::error_to_string(started.error()) << '\n';
        return;
    }

    auto ready = manager.wait_started(*started);
    if (ready) {
        const auto snapshot = manager.inspect(*started);
        std::cout << "Official Module " << type << " ready: "
                  << (snapshot ? snapshot->name : "<exited>") << " MID=" << *started << '\n';
        return;
    }

    std::cerr << "Official Module " << type << " MID=" << *started << " failed: "
              << vinc::error_to_string(ready.error()) << '\n';
    const auto snapshot = manager.inspect(*started);
    if (snapshot && snapshot->last_failure) {
        std::cerr << "Module failure: " << vinc::error_to_string(snapshot->last_failure->error) << '\n';
    }

    // A wait timeout alone does not cancel startup. Request an explicit stop;
    // supervision keeps owning any child whose OS exit is not yet confirmed.
    auto stopped = manager.stop_module(*started, std::chrono::steady_clock::now() + stop_timeout);
    if (!stopped) {
        std::cerr << "Official Module cleanup " << type << " MID=" << *started << ": "
                  << vinc::error_to_string(stopped.error()) << '\n';
    }
}

vinc::Result<void> run_services(StartupOptions& options) {
    // Both roles use this executable. Query the OS instead of trusting argv[0],
    // cwd or a producer build path; only central creates Manager and Tcl.
    auto native_executable = vinc::osal::executable_path();
    if (!native_executable) {
        return std::unexpected(native_executable.error());
    }

    auto executable_text = vinc::osal::path_to_utf8(*native_executable);
    if (!executable_text) {
        return std::unexpected(executable_text.error());
    }

    auto initialized = vinc::shell::initialize_tcl(executable_text->c_str());
    if (!initialized) {
        return initialized;
    }
    auto directory = vinc::shell::executable_directory();
    if (!directory) {
        return std::unexpected(directory.error());
    }
#ifdef VINC_COMPOSED_APP
    auto owner = vinc::detail::ManagerAccess::create(std::string(vinc::app::instance_name), {},
        *native_executable);
#else
    auto owner = vinc::ModuleManager::create({}, *native_executable);
#endif
    if (!owner) {
        return std::unexpected(owner.error());
    }

    auto& manager = **owner;
    auto logging = vinc::detail::ManagerAccess::configure_log(manager, *directory / "vinc_data");
    if (!logging) {
        std::cerr << "Log file: " << vinc::error_to_string(logging.error()) << '\n';
    }
    auto parameters = vinc::detail::ManagerAccess::configure_params(manager, *directory / "vinc_data");
    if (!parameters) {
        // Persistence failure leaves the live service available with registered defaults.
        std::cerr << "Parameter file: " << vinc::error_to_string(parameters.error()) << '\n';
    }
    // Shell must be destroyed before its manager. Reserve Shell/Tcl command names
    // before registering module factories, so collisions fail at registration.
    auto shell_owner = vinc::Shell::create(manager, std::cout);
    if (!shell_owner) {
        return std::unexpected(shell_owner.error());
    }
    auto& shell = **shell_owner;
#ifdef VINC_COMPOSED_APP
    auto registered = vinc::register_built_modules(manager, *directory);
#else
    auto registered = vinc::register_built_modules(manager);
#endif
    if (!registered) {
        return std::unexpected(registered.error());
    }

    auto discovered = vinc::discover_python_modules(manager, *directory / "data" / "module",
        *directory / "python" / "vinc" / "bootstrap.py", options.python);
    if (!discovered) {
        std::cerr << "Python discovery: " << vinc::error_to_string(discovered.error()) << '\n';
    } else {
        for (const auto& issue : discovered->issues) {
            auto path = vinc::osal::path_to_utf8(issue.path);
            std::cerr << "Python module " << (path ? *path : "<invalid path>") << ": "
                      << vinc::error_to_string(issue.error) << '\n';
        }
    }

    const auto endpoint = shell.start_socket_shell(std::move(options.network));
    if (!endpoint) {
        return std::unexpected(endpoint.error());
    }
    std::cout << "Socket Shell listening on "
              << std::visit(
                     [](const auto& ip) {
                         return ip.to_string();
                     },
                     endpoint->address)
              << ':' << endpoint->port << " (Raw TCP)\n"
              << std::flush;

    start_official(manager, "webUi", vinc::config::ui_stop_timeout);
    start_official(manager, "bagRecorder", vinc::config::bag_stop_timeout);

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

        auto result = run_services(options);
        if (!result) {
            std::cerr << "Shell startup failed: " << vinc::error_to_string(result.error()) << '\n';
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

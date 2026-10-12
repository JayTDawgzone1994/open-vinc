#include <vinc/osal/path.hpp>
#include <vinc/module/detail/manager_access.hpp>
// Central executable entry point: parse -S, locate Tcl resources, reserve command names,
// register compiled module types, then enter the Shell/deployment loop.

#include <vinc/shell/detail/shell.hpp>
#include <vinc/shell/detail/tcl_runtime.hpp>
#include <module_catalog.hpp>
#include <vinc/config.hpp>
#include <iostream>
#include <charconv>
#include <limits>
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
    "Usage: Vinc [--shell-ip IP] [--shell-port PORT] [--shell-disconnect-timeout SECONDS]\n"
    "            [--shell-send-timeout SECONDS] [--shell-max-connections 1..8] [-S file.tcl [args...]]\n"
    "Defaults: 127.0.0.1:5500, 30-second disconnect/send timeouts, 8 connections.\n"
    "Raw TCP, no authentication or encryption. Port 0 selects an ephemeral port.\n";

struct StartupOptions {
    std::filesystem::path script;
    std::vector<std::string> script_arguments;
    vinc::shell::SocketShellOptions network;
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

vinc::Result<void> run_services(const char* executable, StartupOptions& options) {
    auto initialized = vinc::shell::initialize_tcl(executable);
    if (!initialized) {
        return initialized;
    }
    auto owner = vinc::ModuleManager::create();
    if (!owner) {
        return std::unexpected(owner.error());
    }
    auto& manager = **owner;
    auto directory = vinc::shell::executable_directory();
    if (!directory) {
        return std::unexpected(directory.error());
    }
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
    auto registered = vinc::register_built_modules(manager);
    if (!registered) {
        return std::unexpected(registered.error());
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

    return shell.run(std::move(options.script), std::move(options.script_arguments));
}
}

int application(int argc, char** argv) {
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

        auto result = run_services(argv[0], options);
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

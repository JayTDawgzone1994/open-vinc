// Central executable entry point: parse -S, locate Tcl resources, reserve command names,
// register compiled module types, then enter the Shell/deployment loop.

#include <stem_shell/stem_shell.hpp>
#include <stem_shell/tcl_runtime.hpp>
#include <module_catalog.hpp>
#include <iostream>
#include <charconv>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

int application(int argc, char** argv) {
    try {
        std::filesystem::path script;
        std::vector<std::string> script_args;
        vinc::shell::SocketShellOptions network;
        const char* usage = "Usage: Vinc [--shell-ip IP] [--shell-port PORT] [--shell-disconnect-timeout SECONDS]\n"
            "            [--shell-send-timeout SECONDS] [--shell-max-connections 1..8] [-S file.tcl [args...]]\n"
            "Defaults: 127.0.0.1:5500, 30-second disconnect/send timeouts, 8 connections.\n"
            "Raw TCP, no authentication or encryption. Port 0 selects an ephemeral port.\n";
        for (int i = 1; i < argc; ++i) {
            const std::string_view name(argv[i]);
            if (name == "--help") { std::cout << usage; return 0; }
            if (++i == argc || !*argv[i]) { std::cerr << usage; return 2; }
            if (name == "-S") {
                script = std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(argv[i])));
                script_args.assign(argv + i + 1, argv + argc); break;
            }
            if (name == "--shell-ip") { network.ip = argv[i]; continue; }
            const std::string_view value(argv[i]);
            std::uint32_t number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) { std::cerr << usage; return 2; }
            if (name == "--shell-port" && number <= 65535) network.port = static_cast<std::uint16_t>(number);
            else if (name == "--shell-disconnect-timeout" && number <= 86400) network.disconnect_timeout = std::chrono::seconds(number);
            else if (name == "--shell-send-timeout" && number >= 1 && number <= 86400) network.send_timeout = std::chrono::seconds(number);
            else if (name == "--shell-max-connections" && number >= 1 && number <= 8) network.max_connections = number;
            else { std::cerr << usage; return 2; }
        }
        vinc::shell::initialize_tcl(argv[0]);
        vinc::ModuleManager manager;
        // Reserve Shell and Tcl names before registering factories, so collisions fail
        // at registration instead of making a module command silently unreachable.
        vinc::StemShell shell(manager, std::cout);
        vinc::register_built_modules(manager);
        const auto endpoint = shell.start_socket_shell(std::move(network));
        std::cout << "Socket Shell listening on " << std::visit([](const auto& ip) { return ip.to_string(); }, endpoint.address)
            << ':' << endpoint.port << " (Raw TCP)\n" << std::flush;
        shell.run(std::move(script), std::move(script_args)); return 0;
    } catch (const std::exception& e) {
        std::cerr << "Shell startup failed: " << e.what() << '\n'; return 1;
    }
}
#ifdef _WIN32
// Windows supplies UTF-16 arguments. Convert once to the UTF-8 contract used by
// the framework and Tcl, including non-ASCII script paths and arguments.
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!length) return 2;
        std::string value(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), length, nullptr, nullptr);
        value.pop_back(); args.push_back(std::move(value));
    }
    std::vector<char*> pointers;
    for (auto& arg : args) pointers.push_back(arg.data());
    return application(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return application(argc, argv); }
#endif

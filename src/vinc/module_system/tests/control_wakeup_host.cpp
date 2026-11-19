#include "control_wakeup_trace.hpp"
#include <vinc/module/detail/module_runtime.hpp>
#include <vinc/osal/path.hpp>
#include <cstdlib>
#include <iostream>

namespace vinc::test::control_wakeup {
void frame_event(Point point, control::Role role, std::uint64_t generation,
                 const control::Frame& frame) {
    trace.record(point, role, generation, frame);
}
}

namespace {
int run(const std::vector<std::string>& arguments) {
    const auto result = vinc::run_module_process(arguments);
    // Runtime teardown joins the I/O thread before writing the trace.
#ifdef _WIN32
    char* environment_value = nullptr;
    std::size_t environment_size = 0;
    if (_dupenv_s(&environment_value, &environment_size, "VINC_TEST_CONTROL_TRACE") != 0) {
        return 65;
    }
    const std::unique_ptr<char, decltype(&std::free)> owned_path(environment_value, &std::free);
    const auto* trace_path = owned_path.get();
#else
    const auto* trace_path = std::getenv("VINC_TEST_CONTROL_TRACE");
#endif
    if (!trace_path) {
        std::cerr << "Missing test trace path\n";
        return 65;
    }

    auto path = vinc::osal::path_from_utf8(trace_path);
    if (!path) {
        std::cerr << vinc::error_to_string(path.error()) << '\n';
        return 65;
    }

    try {
        vinc::test::control_wakeup::trace.write(*path);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 65;
    }
    return result;
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) {
        auto argument = vinc::osal::path_to_utf8(std::filesystem::path(argv[index]));
        if (!argument) {
            return 64;
        }
        arguments.push_back(std::move(*argument));
    }
    return run(arguments);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif

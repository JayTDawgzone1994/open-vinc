#include <vinc/shell/detail/startup_report.hpp>
#include <vinc/module/error.hpp>
#include <vinc/osal/error.hpp>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
}

int main() {
    try {
        vinc::shell::StartupReport report;
        const auto error = vinc::wrap_error(vinc::module_system::ErrorCode::HookFailed,
            vinc::module_system::ErrorOperation::StartHook,
            vinc::Error{vinc::osal::ErrorCode::PermissionDenied, vinc::osal::ErrorOperation::OpenFileLock});
        report.entries.push_back({"Web UI", "initialization hook failed (MID=1)", error});
        report.entries.push_back({"Bag Recorder", "idle", {}});
        report.diagnostics_available = true;
        std::ostringstream plain;
        report.write(plain, false);
        check(plain.str().find("[FAIL] Web UI failed -") != std::string::npos, "failure summary");
        check(plain.str().find("[OK] Bag Recorder ready - idle") != std::string::npos, "success summary");
        check(plain.str().find("1 startup step failed") != std::string::npos, "failure count");
        check(plain.str().find('\x1b') == std::string::npos, "redirected output contains ANSI");
        check(plain.str().find("domain=") == std::string::npos, "logged details duplicated on screen");

        std::ostringstream colored;
        report.write(colored, true);
        check(colored.str().find("\x1b[31m\xe2\x9d\x8c\x1b[39m Web UI") != std::string::npos,
              "failure marker/color reset");
        check(colored.str().find("\x1b[32m\xe2\x9c\x85\x1b[39m Bag Recorder") != std::string::npos,
              "success marker/color reset");

        report.diagnostics_available = false;
        std::ostringstream fallback;
        report.write(fallback, false, false);
        check(fallback.str().find(vinc::error_to_string(error)) != std::string::npos, "lost fallback cause chain");
        check(fallback.str().find("VINC ready") == std::string::npos, "aborted startup claims ready");

        report.entries.erase(report.entries.begin());
        std::ostringstream healthy;
        report.write(healthy, false);
        check(healthy.str().find("all startup steps completed") != std::string::npos, "healthy summary");
        std::cout << "startup report tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

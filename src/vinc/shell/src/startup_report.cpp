#include <vinc/shell/detail/startup_report.hpp>
#include <ostream>

namespace vinc::shell {
void StartupReport::write(std::ostream& output, bool color, bool ready) const {
    std::size_t failures = 0;
    for (const auto& entry : entries) {
        const bool failed = entry.error.has_value();
        if (failed) {
            ++failures;
        }

        if (color) {
            // Color only the status marker; do not alter subsequent user output.
            output << (failed ? "\x1b[31m\xe2\x9d\x8c" : "\x1b[32m\xe2\x9c\x85") << "\x1b[39m ";
        } else {
            output << (failed ? "[FAIL] " : "[OK] ");
        }

        output << entry.name << (failed ? " failed - " : " ready - ") << entry.description << '\n';
        if (failed && !diagnostics_available) {
            output << "       Details: " << error_to_string(*entry.error) << '\n';
            if (entry.related_error) {
                output << "       Related: " << error_to_string(*entry.related_error) << '\n';
            }
        }
    }

    if (!ready) {
        output << "\nVINC startup aborted.\n" << std::flush;
        return;
    }

    if (failures != 0) {
        output << "\nVINC ready - " << failures << " startup step";
        if (failures != 1) {
            output << 's';
        }

        output << " failed; other services remain available.\n";
        if (diagnostics_available) {
            output << "Details: framework log files; use 'log status' to check logging health.\n";
        }
    } else {
        output << "\nVINC ready - all startup steps completed.\n";
    }

    output << "Type help for commands; exit or Ctrl+C to shut down.\n" << std::flush;
}
}

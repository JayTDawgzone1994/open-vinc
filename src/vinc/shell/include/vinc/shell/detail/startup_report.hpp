#pragma once

#include <vinc/error.hpp>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace vinc::shell {
// Central composition supplies one entry per startup step, not a runtime event queue.
// Errors stay intact for Log; presentation never changes service failure policy.
struct StartupEntry {
    std::string name;
    std::string description;
    std::optional<Error> error;
    // Preserve a distinct supervised failure without printing a second status row.
    std::optional<Error> related_error;
};

struct StartupReport {
    std::vector<StartupEntry> entries;
    bool diagnostics_available = false;

    void write(std::ostream& output, bool color, bool ready = true) const;
};
}

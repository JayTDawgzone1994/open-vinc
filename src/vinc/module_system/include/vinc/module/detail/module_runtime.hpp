#pragma once

// Framework-owned executable bootstrap, called only by the supplied Module entry point.
#include <string>
#include <vector>

namespace vinc {
int run_module_process(const std::vector<std::string>& arguments);
}

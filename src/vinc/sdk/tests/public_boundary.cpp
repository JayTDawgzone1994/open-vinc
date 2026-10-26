// Compile only; never linked. These aliases deliberately conflict with service
// declarations if the SDK starts exposing internal Shell bootstrap APIs again.
#include <vinc/vinc.hpp>

#include <type_traits>

namespace vinc {
using Shell = int;
namespace shell {
using SocketShell = int;
using ScriptService = int;
using TclRuntime = int;
using initialize_tcl = int;
}
}

static_assert(std::is_default_constructible_v<vinc::shell::SocketShellOptions>);
static_assert(std::is_default_constructible_v<vinc::ModuleContractReport>);

#ifdef SPDLOG_COMPILED_LIB
#error "spdlog must remain a private file-backend dependency"
#endif
#ifdef SPDLOG_FMT_EXTERNAL
#error "spdlog usage requirements leaked into SDK consumers"
#endif

#pragma once

#include <vinc/detail/runtime_services.hpp>

// Test-only same-build entry points. No production loader or Plugin ABI is added.
struct BridgePluginApi {
    vinc::Result<void> (*bind)(const vinc::detail::RuntimeServices&) noexcept;
    void (*clear)() noexcept;
    void (*fail)() noexcept;
};

#ifndef BRIDGE_ENTRY
extern "C" const BridgePluginApi* bridge_plugin_a();
extern "C" const BridgePluginApi* bridge_plugin_b();
#endif

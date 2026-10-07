#include "runtime_services_plugin.hpp"
#include <vinc/contract.hpp>

namespace {
void fail() noexcept {
    vinc::contract_index_violation(BRIDGE_REASON, 23, 7);
}

const BridgePluginApi api{
    vinc::detail::bind_runtime_services,
    vinc::detail::clear_runtime_services,
    fail};
}

#if defined(_WIN32)
#define BRIDGE_EXPORT __declspec(dllexport)
#else
#define BRIDGE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" BRIDGE_EXPORT const BridgePluginApi* BRIDGE_ENTRY() {
    return &api;
}

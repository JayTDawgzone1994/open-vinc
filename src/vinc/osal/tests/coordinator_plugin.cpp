#include "coordinator_fixture.hpp"
#include <optional>

namespace {
using namespace vinc;
using namespace vinc::osal::detail;
std::optional<ResourceGuard> held;
int* unload_result = nullptr;

struct UnloadProbe {
    ~UnloadProbe() {
        if (unload_result) {
            // This executes inside native unloading, after explicit work stopped.
            // The Host state/code must still exist and the binding must not clear.
            auto guard = ResourceGuard::try_acquire();
            *unload_result = guard ? 1 : -1;
        }
    }
} unload_probe;

Result<void> enter(bool try_only, Deadline deadline, std::stop_token stop) {
    auto guard = try_only ? ResourceGuard::try_acquire() : ResourceGuard::acquire_until(deadline, stop);
    if (!guard) {
        return std::unexpected(guard.error());
    }

    held.emplace(std::move(*guard));
    return {};
}
void leave() {
    held.reset();
}
void* identity() {
    auto services = resource_coordination();
    return services ? services->context : nullptr;
}
void observe_unload(int* result) {
    unload_result = result;
}
}

#ifdef _WIN32
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" FIXTURE_EXPORT const CoordinatorFixture* coordinator_fixture() {
    static const CoordinatorFixture fixture{bind_resource_coordination, enter, leave, identity, observe_unload};
    return &fixture;
}

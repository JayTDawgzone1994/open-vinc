// This standalone probe checks SDK/toolchain capabilities, not project sources.
#include <atomic>
#include <cstdint>
#include <expected>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

static_assert(sizeof(void*) == 8, "Open-VINC setup targets 64-bit toolchains");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

int main() {
    std::expected<int, int> value = 1;
    std::jthread worker([] {});
    worker.join();
#ifdef _WIN32
    return *value == 1 && GetCurrentProcessId() != 0 ? 0 : 1;
#else
    return *value == 1 ? 0 : 1;
#endif
}

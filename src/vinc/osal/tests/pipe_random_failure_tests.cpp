// This executable supplies random_bytes at link time. Production keeps its real
// OS provider; no runtime hook, global registry or public test API is introduced.
#include <vinc/osal/pipe.hpp>
#include <vinc/osal/random.hpp>
#include <vinc/osal/process_pipe.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
constexpr std::uint32_t injected_status = 0xC0000001u;
constexpr int failed_attempts = 4;
constexpr auto shutdown_timeout = std::chrono::seconds(5);
bool fail_random = true;
int random_calls = 0;
}

namespace vinc::osal {
vinc::Result<void> random_bytes(std::span<std::byte> bytes) {
    ++random_calls;
    if (fail_random) {
        return std::unexpected(vinc::Error{
            ErrorCode::SystemError, ErrorOperation::RandomBytes,
            vinc::NativeError{vinc::NativeErrorSource::NtStatus, injected_status}});
    }

    // Separate simultaneous Debug/Release test processes without calling BCrypt.
    std::fill(bytes.begin(), bytes.end(), std::byte{0xA7});
    const auto process_id = GetCurrentProcessId();
    for (std::size_t index = 0; index < sizeof(process_id) && index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>((process_id >> (index * 8)) & 0xFFu);
    }

    return {};
}
}

int main() {
    using namespace vinc::osal;
    PipeServiceOptions options;
    options.max_endpoints = 2;
    auto service = PipeService::create(options);
    if (!service) {
        return 1;
    }

    for (int attempt = 0; attempt < failed_attempts; ++attempt) {
        auto pipe = service->create_pipe();
        if (pipe || !pipe.error().is(ErrorCode::SystemError) ||
            !pipe.error().is_operation(ErrorOperation::CreatePipe) ||
            pipe.error().native.source != vinc::NativeErrorSource::NtStatus ||
            pipe.error().native.value() != injected_status) {
            std::cerr << "FAIL: Pipe lost the random provider diagnostic\n";
            return 1;
        }

        const auto causes = pipe.error().causes();
        if (causes.size() != 1 || causes.front().domain != vinc::ErrorDomain::Osal ||
            causes.front().operation != static_cast<std::int32_t>(ErrorOperation::RandomBytes) ||
            causes.front().native != pipe.error().native) {
            std::cerr << "FAIL: Pipe lost or duplicated its random failure cause\n";
            return 1;
        }
    }

    auto processes = ProcessService::create();
    if (!processes) {
        return 1;
    }

    SpawnOptions spawn_options;
    // The random provider fails before the executable is validated or launched.
    auto child = spawn_with_pipe(*processes, *service, spawn_options);
    constexpr bool full_chain_fits = vinc::config::error_chain_max_depth >= 3;
    constexpr std::size_t expected_causes = full_chain_fits ? 2 : 1;
    if (child || !child.error().is_operation(ErrorOperation::Spawn) ||
        !child.error().is(ErrorCode::SystemError) || child.error().causes().size() != expected_causes ||
        child.error().truncated() == full_chain_fits ||
        child.error().causes().back().operation != static_cast<std::int32_t>(ErrorOperation::RandomBytes) ||
        child.error().native.value() != injected_status) {
        std::cerr << "FAIL: Spawn lost its Pipe/random failure chain\n";
        return 1;
    }

    if (full_chain_fits &&
        child.error().causes().front().operation != static_cast<std::int32_t>(ErrorOperation::CreatePipe)) {
        std::cerr << "FAIL: Spawn lost its intermediate Pipe operation\n";
        return 1;
    }

    fail_random = false;
    auto pipe = service->create_pipe();
    constexpr int spawn_random_attempts = 1;
    constexpr int successful_random_attempts = 1;
    if (!pipe || random_calls != failed_attempts + spawn_random_attempts + successful_random_attempts) {
        std::cerr << "FAIL: failed creation retained endpoint credits\n";
        return 1;
    }

    auto stopped = service->shutdown_until(std::chrono::steady_clock::now() + shutdown_timeout);
    if (!stopped) {
        return 1;
    }

    auto processes_stopped = processes->shutdown_until(std::chrono::steady_clock::now() + shutdown_timeout);
    if (!processes_stopped) {
        return 1;
    }

    std::cout << "PASS: Pipe preserves NTSTATUS and rolls back failed creation\n";
    return 0;
}

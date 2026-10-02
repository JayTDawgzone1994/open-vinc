// Named-memory lifetime, access and real cross-process visibility tests.
// Pipe tokens synchronize test steps; SharedMemory itself supplies no message synchronization.

#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/process_pipe.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/resource.h>
#endif

using namespace vinc::osal;
using namespace std::chrono_literals;
static_assert(!std::is_copy_constructible_v<SharedMemory>);
static_assert(!std::is_copy_constructible_v<SharedMemoryMapping>);
static_assert(std::is_nothrow_move_constructible_v<SharedMemory>);
static_assert(std::is_nothrow_move_assignable_v<SharedMemoryMapping>);
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error("OSAL code=" + std::to_string(static_cast<int>(result.error().code)) +
        " operation=" + std::to_string(static_cast<int>(result.error().operation)) + " " + result.error().native.message());
    return std::move(*result);
}
void ok(Result<void> result) {
    if (!result) throw std::runtime_error("OSAL " + result.error().native.message());
}
template<class T> void fails(const Result<T>& result, ErrorCode code, const char* message) {
    require(!result && result.error().code == code, message);
}
Deadline soon() { return std::chrono::steady_clock::now() + 10s; }
std::string unique_name() {
    static std::uint32_t sequence = 0;
    const auto entropy = static_cast<std::uint64_t>(std::random_device{}()) << 32 | std::random_device{}();
    return "s" + std::to_string(entropy) + "_" + std::to_string(++sequence);
}
struct NameCleanup {
    std::string name;
    explicit NameCleanup(std::string n) : name(std::move(n)) {}
    ~NameCleanup() {
#ifndef _WIN32
        (void)SharedMemory::unlink_name(name);
#endif
    }
};
void token(PipeService& service, PipeWriter& writer, std::byte value) {
    IoBuffer buffer(1); buffer.bytes()[0] = value;
    const auto id = take(service.submit_write(writer, std::move(buffer), soon()));
    std::array<IoCompletion, 1> result;
    require(take(service.wait_completions(result, soon())) == 1, "no write completion");
    require(result[0].id == id && result[0].status == IoStatus::Completed && result[0].transferred == 1, "write token failed");
}
void token(PipeService& service, PipeReader& reader, std::byte expected) {
    const auto id = take(service.submit_read(reader, IoBuffer(1), soon()));
    std::array<IoCompletion, 1> result;
    require(take(service.wait_completions(result, soon())) == 1, "no read completion");
    require(result[0].id == id && result[0].status == IoStatus::Completed && result[0].transferred == 1 &&
        result[0].buffer.bytes()[0] == expected, "read token failed");
}
std::byte pattern(std::size_t i) { return static_cast<std::byte>((i * 37 + 11) & 255); }
int helper(const std::string& name, std::size_t size, const std::string& mode, std::uintptr_t parent_address) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
    struct rlimit limit{0, 0}; (void)setrlimit(RLIMIT_CORE, &limit);
#endif
    auto pipes = take(PipeService::create());
    auto pipe = take(take_inherited_pipe(pipes));
    const auto access = mode == "write" || mode == "creator" ? SharedMemoryAccess::ReadWrite : SharedMemoryAccess::ReadOnly;
    auto memory = mode == "creator" ? take(SharedMemory::create(name, size)) : take(SharedMemory::open(name, access));
    auto mapping = take(memory.map(size, access));
    // Two simultaneous views necessarily differ. Select a view different from
    // the parent's address to verify the protocol never depends on raw pointers.
    auto second = take(memory.map(size, access));
    if (reinterpret_cast<std::uintptr_t>(mapping.bytes().data()) == parent_address)
        mapping = std::move(second);
    require(reinterpret_cast<std::uintptr_t>(mapping.bytes().data()) != parent_address, "same address test setup");
    if (mode == "creator") {
        auto output = take(mapping.writable_bytes());
        for (std::size_t i = 0; i < size; ++i) output[i] = pattern(i);
    }
    ok(memory.close());
    token(pipes, pipe.writer, std::byte{1}); // mapped, public handle closed
    token(pipes, pipe.reader, std::byte{2}); // parent has completed initialization
    const auto bytes = mapping.bytes();
    for (std::size_t i = 0; i < size; ++i) require(bytes[i] == pattern(i), "cross-process data mismatch");
    if (mode == "fault") {
        // Isolated helper deliberately bypasses C++ constness to test OS rights.
        *const_cast<volatile std::byte*>(bytes.data()) = std::byte{99};
        return 91; // should never reach this point
    }
    if (mode == "abrupt" || mode == "creator") std::_Exit(17); // no RAII: OS releases process references
    if (mode == "write") {
        auto output = take(mapping.writable_bytes());
        for (auto& value : output) value ^= std::byte{0xff};
    }
    token(pipes, pipe.writer, std::byte{3});
    token(pipes, pipe.reader, std::byte{4}); // parent read complete
    pipe.close();
    ok(pipes.shutdown_until(soon()));
    return 0;
}
struct Harness {
    ProcessService processes = take(ProcessService::create());
    PipeService pipes = take(PipeService::create());
    std::filesystem::path executable;
    std::vector<ProcessId> children;
    explicit Harness(std::filesystem::path path) : executable(std::move(path)) {}
    ~Harness() {
        for (auto id : children) (void)processes.terminate(id);
        (void)pipes.shutdown_until(soon());
        (void)processes.shutdown_until(soon());
    }
    ProcessWithPipe spawn(const std::string& name, std::size_t size, const char* mode, const std::byte* address) {
        SpawnOptions options;
        options.executable = executable;
        options.arguments = {"--helper", name, std::to_string(size), mode,
            std::to_string(reinterpret_cast<std::uintptr_t>(address))};
        auto child = take(spawn_with_pipe(processes, pipes, options));
        children.push_back(child.process.id());
        return child;
    }
    ExitStatus wait(ProcessWithPipe& child) {
        auto status = take(child.process.wait_until(soon()));
        child.pipe.close();
        ok(processes.forget(child.process.id()));
        return status;
    }
};
void basic_tests() {
    const auto invalid_access = static_cast<SharedMemoryAccess>(99);
    for (const auto& name : {std::string{}, std::string("a/b"), std::string("a\\b"), std::string("a\0b", 3), std::string(25, 'x')}) {
        fails(SharedMemory::create(name, 1), ErrorCode::InvalidArgument, "invalid create name");
        fails(SharedMemory::open(name, SharedMemoryAccess::ReadOnly), ErrorCode::InvalidArgument, "invalid open name");
        fails(SharedMemory::unlink_name(name), ErrorCode::InvalidArgument, "invalid unlink name");
    }
    NameCleanup cleanup(unique_name());
    fails(SharedMemory::create(cleanup.name, 0), ErrorCode::InvalidArgument, "zero size");
    fails(SharedMemory::create(cleanup.name, UINT64_MAX), ErrorCode::InvalidArgument, "size overflow");
    auto absent = SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadOnly);
    fails(absent, ErrorCode::NotFound, "missing name");
    require(absent.error().native.value() != 0 && absent.error().operation == Operation::OpenSharedMemory, "native error missing");
    fails(SharedMemory::open(cleanup.name, invalid_access), ErrorCode::InvalidArgument, "invalid access");
    auto memory = take(SharedMemory::create(cleanup.name, 8192));
    auto duplicate = SharedMemory::create(cleanup.name, 4096);
    fails(duplicate, ErrorCode::AlreadyExists, "duplicate not rejected");
    require(duplicate.error().native.value() != 0, "duplicate native error missing");
    fails(memory.map(0, SharedMemoryAccess::ReadOnly), ErrorCode::InvalidArgument, "zero mapping");
    fails(memory.map(UINT64_MAX, SharedMemoryAccess::ReadOnly), ErrorCode::InvalidArgument, "map overflow");
    fails(memory.map(8193, SharedMemoryAccess::ReadOnly), ErrorCode::InvalidArgument, "creator size bound");
    fails(memory.map(1, invalid_access), ErrorCode::InvalidArgument, "map invalid access");
    auto writer = take(memory.map(8192, SharedMemoryAccess::ReadWrite));
    require(std::all_of(writer.bytes().begin(), writer.bytes().end(), [](auto b) { return b == std::byte{0}; }), "not zero initialized");
    auto ro = take(SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadOnly));
    fails(ro.map(1, SharedMemoryAccess::ReadWrite), ErrorCode::PermissionDenied, "readonly handle upgraded");
    require(!ro.map(1024 * 1024, SharedMemoryAccess::ReadOnly), "oversized native mapping accepted");
    auto reader = take(ro.map(8192, SharedMemoryAccess::ReadOnly));
    fails(reader.writable_bytes(), ErrorCode::PermissionDenied, "readonly view upgraded");
    take(writer.writable_bytes())[8191] = std::byte{73};
    require(reader.bytes()[8191] == std::byte{73}, "views do not share storage");
    auto prefix = take(memory.map(3, SharedMemoryAccess::ReadOnly));
    require(prefix.bytes().size() == 3, "prefix extent lost");
    auto moved = std::move(memory);
    require(!memory && moved, "handle move");
    ok(moved.close()); ok(moved.close()); ok(ro.close());
    require(reader.bytes()[8191] == std::byte{73}, "close invalidated view");
    fails(moved.map(1, SharedMemoryAccess::ReadOnly), ErrorCode::Closed, "map closed handle");
    auto moved_view = std::move(reader);
    require(!reader && moved_view, "mapping move");
    fails(reader.writable_bytes(), ErrorCode::Closed, "moved view still writable");
#ifdef _WIN32
    fails(SharedMemory::unlink_name(cleanup.name), ErrorCode::Unsupported, "Windows unlink contract");
#else
    ok(SharedMemory::unlink_name(cleanup.name));
    fails(SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadOnly), ErrorCode::NotFound, "unlinked name opens");
    fails(SharedMemory::unlink_name(cleanup.name), ErrorCode::NotFound, "repeat unlink contract");
    // A recreated name is a new object; existing mappings must retain the old one.
    auto replacement = take(SharedMemory::create(cleanup.name, 8192));
    auto replacement_view = take(replacement.map(8192, SharedMemoryAccess::ReadOnly));
    require(replacement_view.bytes()[8191] == std::byte{0}, "recreated object aliases old one");
#endif
    require(moved_view.bytes()[8191] == std::byte{73}, "unlink changed existing view");
    ok(writer.unmap()); ok(prefix.unmap()); ok(moved_view.unmap()); ok(moved_view.unmap());
    require(moved_view.bytes().empty(), "unmapped view exposes bytes");
#ifdef _WIN32
    fails(SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadOnly), ErrorCode::NotFound, "last reference did not release name");
#endif
    std::cout << "PASS: validation, exclusive creation, permissions, mapping lifetime, names\n";
}
void process_tests(const std::filesystem::path& executable) {
    Harness test(executable);
    constexpr std::size_t size = 2 * 1024 * 1024 + 17; // non-page-aligned large payload
    for (const char* mode : {"read", "write", "abrupt", "fault", "kill"}) {
        NameCleanup cleanup(unique_name());
        auto memory = take(SharedMemory::create(cleanup.name, size));
        auto view = take(memory.map(size, SharedMemoryAccess::ReadWrite));
        auto child = test.spawn(cleanup.name, size, mode, view.bytes().data());
        token(test.pipes, child.pipe.reader, std::byte{1});
        auto output = take(view.writable_bytes());
        for (std::size_t i = 0; i < size; ++i) output[i] = pattern(i);
        ok(memory.close());
#ifndef _WIN32
        ok(SharedMemory::unlink_name(cleanup.name)); // child already mapped; name is no longer needed
#endif
        if (std::string_view(mode) == "kill") {
            take(child.process.terminate()); // child is waiting on a pipe, holding its mapping
            (void)test.wait(child);
        } else {
            token(test.pipes, child.pipe.writer, std::byte{2});
            if (std::string_view(mode) == "read" || std::string_view(mode) == "write") {
                token(test.pipes, child.pipe.reader, std::byte{3});
                for (std::size_t i = 0; i < size; ++i) {
                    const auto expected = std::string_view(mode) == "write" ? pattern(i) ^ std::byte{0xff} : pattern(i);
                    require(output[i] == expected, "child write not visible / read changed memory");
                }
                token(test.pipes, child.pipe.writer, std::byte{4});
                const auto status = test.wait(child);
                require(status.kind == ExitKind::Code && status.value == 0, "normal helper failed");
            } else {
                const auto status = test.wait(child);
                if (std::string_view(mode) == "abrupt")
                    require(status.kind == ExitKind::Code && status.value == 17, "abrupt helper failed");
                else {
#ifdef _WIN32
                    require(status.kind == ExitKind::Code && status.value == 0xc0000005u, "readonly write was not blocked by OS");
#else
                    require(status.kind == ExitKind::Signal && (status.value == SIGSEGV || status.value == SIGBUS), "readonly write was not blocked by OS");
#endif
                }
            }
        }
        require(output.back() == (std::string_view(mode) == "write" ? pattern(size - 1) ^ std::byte{0xff} : pattern(size - 1)), "child exit invalidated parent data");
        ok(view.unmap());
#ifdef _WIN32
        fails(SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadOnly), ErrorCode::NotFound, "child leaked mapping reference");
#endif
    }
    std::cout << "PASS: distinct-address cross-process exchange, readonly fault, abrupt exit, forced termination\n";
    {
        NameCleanup cleanup(unique_name());
        auto child = test.spawn(cleanup.name, size, "creator", nullptr);
        token(test.pipes, child.pipe.reader, std::byte{1});
        auto memory = take(SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadOnly));
        auto view = take(memory.map(size, SharedMemoryAccess::ReadOnly));
        ok(memory.close());
        token(test.pipes, child.pipe.writer, std::byte{2});
        const auto status = test.wait(child);
        require(status.kind == ExitKind::Code && status.value == 17, "creator exit failed");
        for (std::size_t i = 0; i < size; ++i) require(view.bytes()[i] == pattern(i), "creator exit invalidated peer mapping");
        ok(view.unmap());
#ifdef _WIN32
        fails(SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadOnly), ErrorCode::NotFound, "creator exit retained object");
#else
        auto retained = take(SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadOnly));
        ok(retained.close()); // POSIX name persists until the designated owner unlinks
#endif
    }
    std::cout << "PASS: creator abrupt exit preserves peer mapping; platform-specific name lifetime\n";
}
void churn_tests() {
#ifdef _WIN32
    DWORD before = 0; require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "handle count");
#endif
    for (int i = 0; i < 64; ++i) {
        NameCleanup cleanup(unique_name());
        {
            auto first = take(SharedMemory::create(cleanup.name, 4096));
            auto second = take(SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadWrite));
            auto a = take(first.map(4096, SharedMemoryAccess::ReadWrite));
            auto b = take(second.map(4096, SharedMemoryAccess::ReadOnly));
            first = std::move(second); // releases the replaced handle
            a = std::move(b); // unmaps the replaced view
            require(!second && !b && first && a, "move assignment ownership");
        } // exercise destructor-only cleanup
#ifdef _WIN32
        fails(SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadOnly), ErrorCode::NotFound, "RAII did not release object");
#else
        ok(SharedMemory::unlink_name(cleanup.name));
        fails(SharedMemory::open(cleanup.name, SharedMemoryAccess::ReadOnly), ErrorCode::NotFound, "POSIX name cleanup");
#endif
    }
#ifdef _WIN32
    DWORD after = 0; require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0 && after <= before + 1, "handle leak");
#endif
    std::cout << "PASS: move assignment, repeated RAII cleanup\n";
}
int main(int argc, char** argv) {
    try {
        if (argc == 6 && std::string_view(argv[1]) == "--helper")
            return helper(argv[2], std::stoull(argv[3]), argv[4], std::stoull(argv[5]));
        require(argc == 2, "expected own executable path");
        basic_tests();
        process_tests(std::filesystem::absolute(argv[1]));
        churn_tests();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

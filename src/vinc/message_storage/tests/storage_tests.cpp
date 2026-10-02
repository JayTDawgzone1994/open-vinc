// Arena contract tests for scalar/nested arrays, fixed loans and malformed images.
// Also compare randomized edits with local reference data and open images in another process.

#include <vinc/message/storage.hpp>
#include "framework_access.hpp"
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/process.hpp>
#include <bit>
#include <iostream>
#include <random>
#include <string>
#include <type_traits>

using namespace vinc::message;
using namespace std::chrono_literals;
using Framework = vinc::message::detail::FrameworkAccess;
namespace os = vinc::osal;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class T, class E> T take(std::expected<T, E> value) {
    if (!value) throw std::runtime_error("operation failed: " + std::to_string(static_cast<int>(value.error().code)));
    return std::move(*value);
}
template<class E> void ok(std::expected<void, E> value) {
    if (!value) throw std::runtime_error("operation failed: " + std::to_string(static_cast<int>(value.error().code)));
}
template<class T> void fails(const Result<T>& value, ErrorCode code, const char* text) {
    require(!value && value.error().code == code, text);
}
template<class F> void stale(F&& function) {
    bool caught = false;
    try { function(); } catch (const AccessError& error) { caught = error.error().code == ErrorCode::StaleHandle || error.error().code == ErrorCode::Closed; }
    require(caught, "old access did not fail");
}
std::vector<std::byte> copy_bytes(const Snapshot& image) { return {image.bytes().begin(), image.bytes().end()}; }
void unchanged(const Snapshot& before, const Snapshot& after) {
    require(copy_bytes(before) == copy_bytes(after), "failure changed logical contents");
}
void scalar_tests() {
    MessageArray<std::uint32_t> array;
    for (std::uint32_t i = 0; i < 10000; ++i) ok(array.push_back(i * 7));
    require(array.size() == 10000 && array.capacity() >= array.size(), "scalar growth");
    array[5] = 123;
    require(take(array.at(5)) == 123, "proxy assignment");
    auto proxy = array[5]; auto iterator = array.begin();
    ok(array.resize(20)); stale([&] { (void)static_cast<std::uint32_t>(proxy); }); stale([&] { (void)*iterator; });
    ok(array.resize(100));
    require(take(array.at(20)) == 0 && take(array.at(99)) == 0, "regrowth was not initialized");
    ok(array.reserve(20000));
    const auto compact = take(array.snapshot());
    require(compact.bytes().size() < 1000, "snapshot copied reserve instead of live elements");
    auto relocated = take(Snapshot::copy_from(compact.bytes()));
    auto view = take(relocated.array<std::uint32_t>(0));
    require(view[5] == 123 && view[19] == 19 * 7 && view[99] == 0, "scalar relocation");
    std::uint64_t total = 0; for (auto value : view) total += value;
    require(total > 0, "readonly iteration");
    fails(relocated.array<float>(0), ErrorCode::TypeMismatch, "wrong scalar type accepted");
    fails(array.at(100), ErrorCode::OutOfRange, "bounds");
    auto before = take(array.snapshot());
    fails(array.resize(UINT64_MAX), ErrorCode::SizeOverflow, "overflow resize");
    unchanged(before, take(array.snapshot()));
    ok(array.shrink_to_fit());
    require(array.capacity() == array.size(), "shrink_to_fit did not release reserve");
    unchanged(before, take(array.snapshot()));
    auto moved_view = std::move(view);
    require(moved_view[5] == 123, "moved immutable view lost data");
    stale([&] { (void)view[0]; });

    MessageArray<bool> bits;
    std::vector<bool> input{true, false, true, true};
    ok(bits.copy_from(input)); bits[1] = true;
    require(take(bits.to_vector()) == std::vector<bool>({true, true, true, true}), "vector<bool> adaptation");
    auto bool_snapshot = take(bits.snapshot());
    require(take(bool_snapshot.array<bool>(0))[2], "bool snapshot");

    Message scalars;
    auto signed_values = take(scalars.add_array<std::int64_t>());
    ok(signed_values.copy_from({INT64_MIN, -1, 0, INT64_MAX}));
    auto floating = take(scalars.add_array<double>());
    const double nan = std::bit_cast<double>(std::uint64_t{0x7ff8000000000001});
    ok(floating.copy_from({-0.0, 1.5, nan}));
    auto result = take(scalars.snapshot());
    require(take(result.array<std::int64_t>(0))[0] == INT64_MIN, "signed representation");
    auto doubles = take(result.array<double>(1));
    require(std::bit_cast<std::uint64_t>(doubles[0]) == std::bit_cast<std::uint64_t>(-0.0) &&
        std::bit_cast<std::uint64_t>(doubles[2]) == std::bit_cast<std::uint64_t>(nan), "floating bits changed");
    std::cout << "PASS: local growth, initialization, bool, signed/floating scalars, compact snapshots\n";
}
void nested_tests() {
    Message message;
    auto tree = take(message.add_array<MessageArray<MessageArray<std::int32_t>>>());
    auto sibling = take(message.add_array<std::uint8_t>());
    ok(tree.resize(3));
    auto& branch = tree[1]; ok(branch.resize(2));
    auto& leaf = branch[0]; ok(leaf.copy_from({10, 20, 30}));
    ok(branch[1].copy_from({40, 50}));
    ok(sibling.resize(1024 * 1024)); sibling[999] = 77;
    require(take(leaf.at(2)) == 30, "array handle lost during sibling relocation");
    ok(branch.push_back(leaf)); // deep copy within the same message
    leaf[0] = 99;
    require(take(branch[2].at(0)) == 10, "nested push aliased source");
    auto& old_leaf = branch[2];
    ok(branch.resize(1));
    fails(old_leaf.at(0), ErrorCode::StaleHandle, "removed nested handle survived");
    ok(branch.resize(3));
    fails(old_leaf.at(0), ErrorCode::StaleHandle, "reused descriptor revived old handle");
    require(branch[2].empty(), "new child retained removed contents");
    auto snapshot = take(message.snapshot());
    auto packed = take(Snapshot::copy_from(snapshot.bytes()));
    auto readonly = take(packed.array<MessageArray<MessageArray<std::int32_t>>>(0));
    require(readonly[1][0][0] == 99 && readonly[1][2].empty(), "nested relocation");
    require(take(packed.array<std::uint8_t>(1))[999] == 77, "sibling relocation");
    auto usage_before = take(message.usage());
    ok(tree.clear());
    auto usage_after = take(message.usage());
    require(usage_after.compact_bytes < usage_before.compact_bytes, "nested children not reclaimed");
    fails(leaf.at(0), ErrorCode::StaleHandle, "descendant survived clear");

    MessageArray<MessageArray<std::uint16_t>> standalone;
    MessageArray<std::uint16_t> source; ok(source.copy_from({1, 2, 65535}));
    ok(standalone.push_back(source)); source[0] = 7;
    require(take(standalone[0].at(0)) == 1, "cross-message nested copy aliased");
    std::cout << "PASS: multiple roots, nested growth, deep copy, subtree removal, stale generations\n";
}
// Exercise fixed byte budgets through the private framework seam. Failed edits must
// leave the previous snapshot unchanged, and sealed/revoked handles must reject access.
void fixed_tests() {
    constexpr std::size_t limit = 1024 * 1024;
    auto backing = std::make_shared<std::vector<std::byte>>(limit);
    auto fixed = take(Framework::borrow(*backing, backing, limit));
    auto array = take(fixed.add_array<std::uint8_t>());
    ok(array.resize(123)); array[100] = 42;
    auto before = take(fixed.snapshot()); const auto raw_before = *backing;
    fails(array.resize(2 * limit), ErrorCode::CapacityExceeded, "fixed resize exceeded budget");
    require(*backing == raw_before, "failed resize modified backing bytes");
    unchanged(before, take(fixed.snapshot()));
    auto nested = take(fixed.add_array<MessageArray<std::uint64_t>>());
    ok(nested.resize(2)); ok(nested[0].resize(10)); nested[0][4] = 1234;
    before = take(fixed.snapshot());
    fails(nested[1].resize(limit), ErrorCode::CapacityExceeded, "nested budget not shared");
    unchanged(before, take(fixed.snapshot()));
    MessageArray<std::uint64_t> large; ok(large.resize(limit / 4));
    fails(nested.push_back(large), ErrorCode::CapacityExceeded, "nested import exceeded budget");
    unchanged(before, take(fixed.snapshot()));
    auto sealed = take(Framework::seal(fixed));
    require(sealed.bytes().data() == backing->data(), "seal is not in borrowed storage");
    fails(array.push_back(1), ErrorCode::Closed, "sealed writer remains active");
    fails(nested.resize(3), ErrorCode::Closed, "sealed nested writer active");
    auto view = take(sealed.array<MessageArray<std::uint64_t>>(1));
    require(view[0][4] == 1234, "sealed nested data");
    auto local_copy = take(Snapshot::copy_from(sealed.bytes()));
    require(local_copy.bytes().data() != sealed.bytes().data(), "public copy_from borrowed external storage");
    require(take(local_copy.array<std::uint8_t>(0))[100] == 42, "local snapshot copy");

    // Exact packed capacity fits even when geometric growth would exceed it.
    Message model; auto model_array = take(model.add_array<std::uint64_t>()); ok(model_array.resize(3));
    const auto exact = take(model.snapshot()).bytes().size();
    auto exact_memory = std::make_shared<std::vector<std::byte>>(exact);
    auto exact_message = take(Framework::borrow(*exact_memory, exact_memory, exact));
    auto exact_array = take(exact_message.add_array<std::uint64_t>());
    ok(exact_array.resize(3)); require(exact_array.capacity() == 3, "growth fallback did not use exact capacity");
    auto empty_before = take(exact_message.snapshot());
    fails(exact_array.resize(5), ErrorCode::CapacityExceeded, "exact boundary overflow");
    unchanged(empty_before, take(exact_message.snapshot()));

    // Bounded scratch rejection must leave both payload and descriptor state unchanged.
    auto scratch_memory = std::make_shared<std::vector<std::byte>>(4096);
    auto scratch = take(Framework::borrow(*scratch_memory, scratch_memory, 256));
    auto scratch_array = take(scratch.add_array<std::uint8_t>());
    const auto old = *scratch_memory;
    fails(scratch_array.resize(1000), ErrorCode::ResourceExhausted, "scratch budget ignored");
    require(scratch_array.empty() && *scratch_memory == old, "scratch failure mutated source");
    Framework::revoke(scratch);
    fails(scratch_array.resize(1), ErrorCode::Closed, "revoked borrow active");
    std::unique_ptr<MessageArray<std::uint8_t>> abandoned;
    {
        auto temporary = take(Framework::borrow(*scratch_memory, scratch_memory, 4096));
        abandoned = std::make_unique<MessageArray<std::uint8_t>>(take(temporary.add_array<std::uint8_t>()));
    }
    fails(abandoned->resize(1), ErrorCode::Closed, "abandoned loan remains writable through alias");

    // Reserved bytes count during a loan; explicit compaction releases them to siblings.
    auto small_memory = std::make_shared<std::vector<std::byte>>(512);
    auto small = take(Framework::borrow(*small_memory, small_memory, 512));
    auto first = take(small.add_array<std::uint8_t>());
    auto second = take(small.add_array<std::uint8_t>());
    ok(first.reserve(300)); ok(first.resize(8)); first[0] = 91;
    fails(second.resize(100), ErrorCode::CapacityExceeded, "reserved bytes not accounted");
    ok(small.compact());
    require(first.capacity() == 8 && take(first.at(0)) == 91, "compact lost data or kept reserve");
    ok(second.resize(100));
    require(take(small.usage()).allocated_bytes <= 512, "compacted budget");
    const auto small_before = *small_memory;
    std::vector<std::uint8_t> oversized_input(1000, 17);
    fails(first.copy_from(oversized_input), ErrorCode::CapacityExceeded, "copy preflight exceeded budget");
    require(*small_memory == small_before && take(first.at(0)) == 91, "copy failure changed bytes");
    Message moved = std::move(small);
    require(take(first.at(0)) == 91, "moving loan revoked its arrays");
    fails(small.usage(), ErrorCode::Closed, "moved-from message active");
    moved = Message{};
    fails(first.at(0), ErrorCode::Closed, "replacing loan did not revoke old arrays");

    // Publisher preflight uses packed bytes, and preserves oversized local source.
    Message local; auto payload = take(local.add_array<std::uint8_t>());
    ok(payload.resize(2 * limit)); payload[123] = 19;
    fails(local.snapshot(limit), ErrorCode::CapacityExceeded, "oversized submission accepted");
    require(payload.size() == 2 * limit && take(payload.at(123)) == 19, "rejected local message consumed");
    ok(payload.resize(100)); // reserve remains > 1 MiB, live data fits
    require(take(local.snapshot(limit)).bytes().size() < limit, "submission compared reserved size");
    std::cout << "PASS: shared byte budget, exact fit, transactional failures, scratch bound, seal/revoke\n";
}
void put64(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.at(at + i) = static_cast<std::byte>((value >> (8 * i)) & 255);
}
void malformed_tests() {
    MessageArray<MessageArray<bool>> array; ok(array.resize(1)); ok(array[0].push_back(true));
    auto valid = take(array.snapshot()); auto original = copy_bytes(valid);
    for (std::size_t length : {std::size_t{0}, std::size_t{63}, original.size() - 1})
        require(!Snapshot::copy_from(std::span(original).first(length)), "truncation accepted");
    for (const auto& [offset, value] : std::vector<std::pair<std::size_t, std::uint64_t>>{
        {0, 0}, {8, 999}, {16, UINT64_MAX}, {24, UINT64_MAX}, {32, UINT64_MAX}, {40, 0}, {48, 0}, {56, 1}, {64, UINT64_MAX},
        {72, 64}, // descriptor payload overlaps root list
        {80, UINT64_MAX}, // count overflow
        {112, 1}, // reserved descriptor bits (v2 descriptors are 48 bytes)
    }) {
        auto broken = original; put64(broken, offset, value);
        require(!Snapshot::copy_from(broken), "malformed image accepted");
    }
    auto invalid_bool = original;
    // Header 64, root 8, descriptors 96, nested index 8, bool payload at 176.
    invalid_bool.at(176) = std::byte{2};
    require(!Snapshot::copy_from(invalid_bool), "noncanonical bool accepted");
    auto cycle = original; put64(cycle, 168, 0);
    require(!Snapshot::copy_from(cycle), "cycle or duplicate ownership accepted");
    auto padding = original; padding.back() = std::byte{1};
    require(!Snapshot::copy_from(padding), "nonzero padding accepted");
    auto extra = original; extra.push_back(std::byte{0});
    require(!Snapshot::copy_from(extra), "trailing bytes accepted");
    std::cout << "PASS: malformed offsets, lengths, ownership, bool, padding and format version\n";
}
void randomized_tests() {
    std::mt19937 random(12345);
    Message message; auto array = take(message.add_array<MessageArray<std::uint32_t>>());
    std::vector<std::vector<std::uint32_t>> reference;
    for (int step = 0; step < 600; ++step) {
        if (reference.empty() || random() % 4 == 0) {
            const auto count = random() % 12;
            ok(array.resize(count)); reference.resize(count);
        } else {
            const auto index = random() % reference.size();
            if (random() % 3 == 0) {
                const auto count = random() % 40;
                ok(array[index].resize(count)); reference[index].resize(count);
            } else {
                const auto value = random(); ok(array[index].push_back(value)); reference[index].push_back(value);
            }
        }
        if (step % 10 == 0) {
            auto packed = take(message.snapshot()); auto relocated = take(Snapshot::copy_from(packed.bytes()));
            auto view = take(relocated.array<MessageArray<std::uint32_t>>(0));
            require(view.size() == reference.size(), "random outer size");
            for (std::size_t i = 0; i < reference.size(); ++i) {
                require(view[i].size() == reference[i].size(), "random inner size");
                for (std::size_t j = 0; j < reference[i].size(); ++j) require(view[i][j] == reference[i][j], "random value mismatch");
            }
        }
    }
    std::cout << "PASS: deterministic nested mutation sequences against std::vector reference\n";
}
std::string shm_name() {
    return "m" + std::to_string(static_cast<std::uint64_t>(std::random_device{}()) << 32 | std::random_device{}());
}
struct Cleanup {
    std::string name;
    ~Cleanup() {
#ifndef _WIN32
        (void)os::SharedMemory::unlink_name(name);
#endif
    }
};
struct ChildGuard {
    os::Process* child;
    ~ChildGuard() { (void)child->terminate(); (void)child->wait_until(std::chrono::steady_clock::now() + 5s); }
};
int helper(const std::string& name, std::size_t size, std::uintptr_t parent_address) {
    auto memory = take(os::SharedMemory::open(name, os::SharedMemoryAccess::ReadOnly));
    auto first = take(memory.map(size, os::SharedMemoryAccess::ReadOnly));
    auto second = take(memory.map(size, os::SharedMemoryAccess::ReadOnly));
    if (reinterpret_cast<std::uintptr_t>(first.bytes().data()) == parent_address) first = std::move(second);
    require(reinterpret_cast<std::uintptr_t>(first.bytes().data()) != parent_address, "relocation setup");
    auto snapshot = take(Snapshot::copy_from(first.bytes())); // the framework's required receive copy
    ok(first.unmap()); ok(second.unmap()); ok(memory.close());
    auto tree = take(snapshot.array<MessageArray<std::uint32_t>>(0));
    require(tree.size() == 3 && tree[2][2] == 300, "cross-process nested image");
    auto bytes = take(snapshot.array<std::uint8_t>(1));
    require(bytes.size() == 2 * 1024 * 1024 && bytes[123456] == 77, "cross-process large array");
    return 0;
}
void process_test(const std::filesystem::path& executable) {
    constexpr std::size_t budget = 4 * 1024 * 1024;
    Cleanup cleanup{shm_name()};
    auto memory = take(os::SharedMemory::create(cleanup.name, budget));
    auto mapping = std::make_shared<os::SharedMemoryMapping>(take(memory.map(budget, os::SharedMemoryAccess::ReadWrite)));
    auto borrowed = take(Framework::borrow(take(mapping->writable_bytes()), mapping, budget));
    auto tree = take(borrowed.add_array<MessageArray<std::uint32_t>>()); ok(tree.resize(3));
    ok(tree[2].copy_from({100, 200, 300}));
    auto bytes = take(borrowed.add_array<std::uint8_t>()); ok(bytes.resize(2 * 1024 * 1024)); bytes[123456] = 77;
    auto snapshot = take(Framework::seal(borrowed));
    mapping.reset(); ok(memory.close()); // snapshot owns the mapping lease
    auto service = take(os::ProcessService::create());
    os::SpawnOptions options;
    options.executable = executable;
    options.arguments = {"--helper", cleanup.name, std::to_string(snapshot.bytes().size()),
        std::to_string(reinterpret_cast<std::uintptr_t>(snapshot.bytes().data()))};
    auto child = take(service.spawn(options)); ChildGuard guard{&child};
    auto status = take(child.wait_until(std::chrono::steady_clock::now() + 10s));
    require(status.kind == os::ExitKind::Code && status.value == 0, "cross-process child failed");
    ok(service.forget(child.id()));
    std::cout << "PASS: private OSAL binding, sealed lease lifetime, independent process receive copy\n";
}
int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--helper") return helper(argv[2], std::stoull(argv[3]), std::stoull(argv[4]));
        require(argc == 2, "expected own executable path");
        scalar_tests(); nested_tests(); fixed_tests(); malformed_tests(); randomized_tests();
        process_test(std::filesystem::absolute(argv[1]));
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

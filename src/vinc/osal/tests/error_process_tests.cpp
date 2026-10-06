// Fixture protocol only: real processes share explicitly encoded error identities.
// This does not change the Module or Message Bus control protocol.
#include <vinc/osal/file_lock.hpp>
#include <vinc/osal/process.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <array>
#include <bit>
#include <filesystem>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
using namespace vinc;
using namespace vinc::osal;
using namespace std::chrono_literals;
constexpr auto process_timeout = 10s;
constexpr std::uint32_t fixture_version = 2;
constexpr std::size_t record_bytes = 24;
constexpr std::size_t envelope_bytes = 12;
constexpr std::size_t length_bytes = 4;
constexpr std::size_t record_count = 4;
constexpr std::size_t slot_bytes =
    length_bytes + envelope_bytes + record_bytes * config::error_chain_max_depth;
constexpr std::size_t shared_bytes = slot_bytes * record_count;
constexpr std::size_t overflow_layers = 3;
constexpr std::size_t byte_bits = 8;

void check(bool condition, const char* description) {
    if (!condition) {
        throw std::runtime_error(description);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    return std::move(*result);
}

void put(std::span<std::byte> bytes, std::size_t& offset, std::uint64_t value, std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
        bytes[offset++] = static_cast<std::byte>((value >> (index * byte_bits)) & 0xff);
    }
}

std::uint64_t get(std::span<const std::byte> bytes, std::size_t& offset, std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset++])) << (index * byte_bits);
    }

    return value;
}

// Fixture v2: version/count/flags, followed by explicit 24-byte records.
// Validate the whole envelope before allocating/reading; never copy C++ layout.
std::optional<Error> decode(std::span<const std::byte> bytes) {
    if (bytes.size() < envelope_bytes + record_bytes) {
        return {};
    }

    std::size_t offset = 0;
    const auto version = get(bytes, offset, sizeof(std::uint32_t));
    const auto count = get(bytes, offset, sizeof(std::uint32_t));
    const auto flags = get(bytes, offset, sizeof(std::uint32_t));
    const auto body_bytes = bytes.size() - envelope_bytes;
    if (version != fixture_version || count == 0 || flags > 1 || body_bytes % record_bytes != 0 ||
        count != body_bytes / record_bytes || (flags != 0 && count < 2)) {
        return {};
    }

    std::vector<ErrorRecord> records;
    records.reserve(static_cast<std::size_t>(count));
    for (std::size_t index = 0; index < count; ++index) {
        ErrorRecord record;
        record.domain = static_cast<ErrorDomain>(get(bytes, offset, sizeof(std::uint32_t)));
        record.code = std::bit_cast<std::int32_t>(
            static_cast<std::uint32_t>(get(bytes, offset, sizeof(std::uint32_t))));
        record.operation = std::bit_cast<std::int32_t>(
            static_cast<std::uint32_t>(get(bytes, offset, sizeof(std::uint32_t))));
        record.native.source = static_cast<NativeErrorSource>(get(bytes, offset, sizeof(std::uint32_t)));
        record.native.code = std::bit_cast<std::int64_t>(get(bytes, offset, sizeof(std::uint64_t)));
        records.push_back(record);
    }
    const auto& current = records.front();
    return Error::from_raw(current.domain, current.code, current.operation, current.native,
                           std::span<const ErrorRecord>(records).subspan(1), flags != 0);
}

void encode_record(std::span<std::byte> bytes, std::size_t& offset, const ErrorRecord& record) {
    put(bytes, offset, static_cast<std::uint32_t>(record.domain), sizeof(std::uint32_t));
    put(bytes, offset, std::bit_cast<std::uint32_t>(record.code), sizeof(std::uint32_t));
    put(bytes, offset, std::bit_cast<std::uint32_t>(record.operation), sizeof(std::uint32_t));
    put(bytes, offset, static_cast<std::uint32_t>(record.native.source), sizeof(std::uint32_t));
    put(bytes, offset, std::bit_cast<std::uint64_t>(record.native.code), sizeof(std::uint64_t));
}

void encode(std::span<std::byte> bytes, const Error& error) {
    const auto count = error.causes().size() + 1;
    check(bytes.size() == envelope_bytes + count * record_bytes, "fixture envelope size");
    std::size_t offset = 0;
    put(bytes, offset, fixture_version, sizeof(std::uint32_t));
    put(bytes, offset, count, sizeof(std::uint32_t));
    put(bytes, offset, error.truncated() ? 1 : 0, sizeof(std::uint32_t));
    encode_record(bytes, offset, error.record());
    for (const auto& cause : error.causes()) {
        encode_record(bytes, offset, cause);
    }
}

std::optional<Error> decode_slot(std::span<const std::byte> bytes) {
    if (bytes.size() < length_bytes) {
        return {};
    }
    std::size_t offset = 0;
    const auto size = get(bytes, offset, length_bytes);
    if (size > bytes.size() - length_bytes) {
        return {};
    }
    return decode(bytes.subspan(length_bytes, static_cast<std::size_t>(size)));
}

Error unknown_error() {
    return Error::from_raw(static_cast<ErrorDomain>(12345), -17, -29,
                           {static_cast<NativeErrorSource>(999), -123456789});
}

Error truncated_error() {
    auto error = unknown_error();
    for (std::size_t layer = 0; layer < config::error_chain_max_depth + overflow_layers; ++layer) {
        error = wrap_error(ErrorCode::SystemError, ErrorOperation::RandomBytes, error);
    }
    return error;
}

int child(const std::string& name) {
    auto memory = take(SharedMemory::open(name, SharedMemoryAccess::ReadWrite));
    auto mapping = take(memory.map(shared_bytes, SharedMemoryAccess::ReadWrite));
    const auto bytes = take(mapping.writable_bytes());
    const auto missing = FileLock::open(std::filesystem::temp_directory_path() / name);
    check(!missing && missing.error().is(ErrorCode::NotFound), "expected native open failure");
    const std::array errors{
        missing.error(),
        Error{ErrorCode::SystemError, ErrorOperation::RandomBytes, {NativeErrorSource::NtStatus, 0xC0000022}},
        wrap_error(ErrorCode::SystemError, ErrorOperation::RandomBytes, unknown_error()), truncated_error()};
    for (std::size_t index = 0; index < errors.size(); ++index) {
        const auto slot = bytes.subspan(index * slot_bytes, slot_bytes);
        const auto size = envelope_bytes + (errors[index].causes().size() + 1) * record_bytes;
        std::size_t offset = 0;
        put(slot, offset, size, length_bytes);
        encode(slot.subspan(length_bytes, size), errors[index]);
    }

    return 0;
}

struct NameCleanup {
    std::string name;

    ~NameCleanup() {
#ifndef _WIN32
        (void)SharedMemory::unlink_name(name);
#endif
    }
};

struct ChildCleanup {
    Process& process;

    ~ChildCleanup() {
        (void)process.terminate();
        (void)process.wait_until(std::chrono::steady_clock::now() + process_timeout);
    }
};

void run_child(const std::filesystem::path& executable, const std::string& name) {
    auto service = take(ProcessService::create());
    SpawnOptions options;
    options.executable = executable;
    options.arguments = {"--child", name};
    auto process = take(service.spawn(options));
    ChildCleanup cleanup{process};
    const auto status = take(process.wait_until(std::chrono::steady_clock::now() + process_timeout));
    check(status.kind == ExitKind::Code && status.value == 0, "child failed");
}

void parent(const std::filesystem::path& executable) {
    const auto entropy = std::uint64_t(std::random_device{}()) << 32 | std::random_device{}();
    const auto name = "e" + std::to_string(entropy);
    auto memory = take(SharedMemory::create(name, shared_bytes));
    NameCleanup cleanup{name};
    run_child(executable, name);

    // The child has exited before this mapping is read. No simultaneous reader/writer
    // accesses or raw object layouts are used; querying text cannot contact the child.
    auto mapping = take(memory.map(shared_bytes, SharedMemoryAccess::ReadOnly));
    const auto bytes = mapping.bytes();
    const auto native = decode_slot(bytes.first(slot_bytes));
    check(native && native->is(ErrorCode::NotFound) && bool(native->native), "native identity lost");
    check(error_to_string(*native).find("Not found") != std::string::npos, "local lookup failed");
    const auto status = decode_slot(bytes.subspan(slot_bytes, slot_bytes));
    check(status && error_to_string(*status).find("native=3:3221225506]") != std::string::npos,
          "NTSTATUS changed across process");
    const auto nested = decode_slot(bytes.subspan(2 * slot_bytes, slot_bytes));
    const auto expected = wrap_error(ErrorCode::SystemError, ErrorOperation::RandomBytes, unknown_error());
    check(nested && *nested == expected, "nested unknown values changed across process");
    check(error_to_string(*nested).find("domain=12345") != std::string::npos, "unknown cause domain lost");
    const auto truncated = decode_slot(bytes.last(slot_bytes));
    check(truncated && *truncated == truncated_error() && truncated->truncated(),
          "truncation state or records changed across process");
    const auto text = error_to_string(*truncated);
    const auto omission = text.find("[intermediate causes omitted]");
    const auto deepest = text.find("domain=12345");
    check(omission != std::string::npos && deepest != std::string::npos && omission < deepest,
          "cross-process omission marker must precede deepest cause");
}

void test_invalid_envelopes() {
    std::array<std::byte, envelope_bytes + record_bytes> invalid{};
    encode(invalid, unknown_error());
    check(!decode(std::span<const std::byte>(invalid).first(invalid.size() - 1)),
          "truncated record accepted");
    invalid[0] = std::byte{0};
    check(!decode(invalid), "unknown fixture version accepted");
    encode(invalid, unknown_error());
    invalid[4] = std::byte{0};
    check(!decode(invalid), "empty chain accepted");
    invalid[4] = std::byte{0xff};
    check(!decode(invalid), "count/length mismatch accepted");
    encode(invalid, unknown_error());
    invalid[8] = std::byte{2};
    check(!decode(invalid), "unknown flags accepted");
    invalid[8] = std::byte{1};
    check(!decode(invalid), "omission without a deepest cause accepted");
    std::vector<std::byte> trailing(invalid.size() + 1);
    encode(std::span<std::byte>(trailing).first(invalid.size()), unknown_error());
    check(!decode(trailing), "trailing bytes accepted");
}

void test_larger_sender() {
    // Simulate a sender with a larger capacity, using valid explicit wire records.
    const auto larger_count = config::error_chain_max_depth + overflow_layers;
    std::vector<std::byte> larger(envelope_bytes + larger_count * record_bytes);
    std::size_t offset = 0;
    put(larger, offset, fixture_version, sizeof(std::uint32_t));
    put(larger, offset, larger_count, sizeof(std::uint32_t));
    put(larger, offset, 0, sizeof(std::uint32_t));
    for (std::size_t index = 0; index < larger_count; ++index) {
        encode_record(larger, offset, unknown_error().record());
    }
    const auto reduced = decode(larger);
    check(reduced && reduced->truncated() && reduced->causes().size() == config::error_chain_max_depth - 1 &&
              reduced->causes().back() == unknown_error().record(),
          "larger sender chain was not bounded on receipt");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--child") {
            return child(argv[2]);
        }

        check(argc == 2, "expected helper executable path");
        const std::string path_text(argv[1]);
        const std::u8string utf8_path(path_text.begin(), path_text.end());
        parent(std::filesystem::path(utf8_path));
        test_invalid_envelopes();
        test_larger_sender();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

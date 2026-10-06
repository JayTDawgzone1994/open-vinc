// Corrupt encoded fields independently of the production schema validator.
#include <vinc_messages/all_messages.hpp>
#include <vinc/message/detail/framework_access.hpp>
#include <future>
#include <stdexcept>

namespace {
using vinc::message::ErrorCode;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

template <class T> T take(vinc::Result<T> result) {
    check(bool(result), "snapshot fixture operation failed");
    return std::move(*result);
}

void ok(vinc::Result<void> result) {
    check(bool(result), "snapshot fixture mutation failed");
}

// The v2 wire format is little-endian. Byte-wise edits avoid native object
// lifetime/alignment assumptions and deliberately do not reuse codec helpers.
std::uint64_t read_integer(const std::vector<std::byte>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes.at(offset + index))) << (index * 8);
    }
    return value;
}

void write_integer(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value,
                   std::size_t width = 8) {
    for (std::size_t index = 0; index < width; ++index) {
        bytes.at(offset + index) = std::byte((value >> (index * 8)) & 0xff);
    }
}

std::size_t descriptor(const std::vector<std::byte>& bytes, std::uint64_t id) {
    return static_cast<std::size_t>(read_integer(bytes, 40) + id * 48);
}

std::uint64_t child(const std::vector<std::byte>& bytes, std::uint64_t id, std::size_t index) {
    const auto payload = read_integer(bytes, descriptor(bytes, id));
    return read_integer(bytes, static_cast<std::size_t>(payload + index * 8));
}

std::vector<std::byte> copy_bytes(const vinc::message::Snapshot& snapshot) {
    return {snapshot.bytes().begin(), snapshot.bytes().end()};
}

template <class T>
void reject_schema(const std::vector<std::byte>& bytes, ErrorCode expected) {
    // A generic-layout failure would not exercise the missing typed boundary.
    auto snapshot = take(vinc::message::Snapshot::copy_from(bytes));
    auto result = snapshot.array<T>(0);
    check(!result && result.error().is(expected) &&
              result.error().is_operation(vinc::message::ErrorOperation::AccessRoot),
          "typed snapshot accepted malformed schema or lost error operation");
}
}

void snapshot_validation_tests() {
    vinc::MessageArray<vinc::Detection> records;
    ok(records.resize(1));
    auto snapshot = take(records.snapshot());
    const auto original = copy_bytes(snapshot);
    const auto root = read_integer(original, 64);
    const auto record = child(original, root, 0);
    const auto id = child(original, record, 0);

    auto wrong_type = original;
    write_integer(wrong_type, descriptor(wrong_type, id) + 16, 9, 4); // uint32 -> float, same width.
    reject_schema<vinc::Detection>(wrong_type, ErrorCode::TypeMismatch);

    auto wrong_order = original;
    const auto record_payload = static_cast<std::size_t>(read_integer(original, descriptor(original, record)));
    const auto confidence = child(original, record, 1);
    write_integer(wrong_order, record_payload, confidence);
    write_integer(wrong_order, record_payload + 8, id);
    reject_schema<vinc::Detection>(wrong_order, ErrorCode::TypeMismatch);

    auto wrong_nested_type = original;
    const auto position = child(original, record, 2);
    const auto x = child(original, position, 0);
    write_integer(wrong_nested_type, descriptor(original, x) + 16, 5, 4); // float -> uint32.
    reject_schema<vinc::Detection>(wrong_nested_type, ErrorCode::TypeMismatch);

    auto wrong_singular = original;
    write_integer(wrong_singular, 12, 2, 4); // Reserved image permits size < capacity.
    write_integer(wrong_singular, descriptor(original, id) + 8, 0);
    reject_schema<vinc::Detection>(wrong_singular, ErrorCode::InvalidData);

    vinc::MessageArray<vinc::MessageArray<vinc::Detection>> nested;
    ok(nested.resize(1));
    ok(nested[0].resize(1));
    auto nested_snapshot = take(nested.snapshot());
    auto nested_bytes = copy_bytes(nested_snapshot);
    const auto nested_root = read_integer(nested_bytes, 64);
    const auto row = child(nested_bytes, nested_root, 0);
    const auto nested_record = child(nested_bytes, row, 0);
    const auto nested_id = child(nested_bytes, nested_record, 0);
    write_integer(nested_bytes, descriptor(nested_bytes, nested_id) + 16, 9, 4);
    reject_schema<vinc::MessageArray<vinc::Detection>>(nested_bytes, ErrorCode::TypeMismatch);

    vinc::MessageArray<vinc::Detection> empty;
    auto empty_snapshot = take(empty.snapshot());
    check(take(empty_snapshot.array<vinc::Detection>(0)).size() == 0, "empty record array rejected");

    vinc::message::Message multiple_roots;
    auto scalar_root = take(multiple_roots.add_array<std::uint32_t>());
    auto record_root = take(multiple_roots.add_array<vinc::Detection>());
    ok(scalar_root.push_back(52));
    ok(record_root.resize(1));
    auto multiple_bytes = copy_bytes(take(multiple_roots.snapshot()));
    const auto second_root = read_integer(multiple_bytes, 72);
    const auto second_record = child(multiple_bytes, second_root, 0);
    const auto second_id = child(multiple_bytes, second_record, 0);
    write_integer(multiple_bytes, descriptor(multiple_bytes, second_id) + 16, 9, 4);
    auto multiple_snapshot = take(vinc::message::Snapshot::copy_from(multiple_bytes));
    check(take(multiple_snapshot.array<std::uint32_t>(0))[0] == 52,
          "schema validation unnecessarily rejected an unrelated valid root");
    auto bad_root = multiple_snapshot.array<vinc::Detection>(1);
    check(!bad_root && bad_root.error().is(ErrorCode::TypeMismatch), "second malformed root accepted");

    // Sealed loans retain spare capacity; validation must traverse live elements
    // without mistaking reserved, zero-filled slots for record children.
    constexpr std::size_t reserved_capacity_bytes = 16 * 1024;
    constexpr std::uint64_t reserved_record_count = 4;
    auto backing = std::make_shared<std::vector<std::byte>>(reserved_capacity_bytes);
    using Framework = vinc::message::detail::FrameworkAccess;
    auto loan = take(Framework::borrow(*backing, backing, reserved_capacity_bytes));
    auto reserved_records = take(loan.add_array<vinc::Detection>());
    ok(reserved_records.resize(1));
    ok(reserved_records.reserve(reserved_record_count));
    auto sealed = take(Framework::seal(loan));
    auto reserved_view = take(sealed.array<vinc::Detection>(0));
    check(reserved_view.size() == 1 && reserved_view.capacity() >= reserved_record_count &&
              reserved_view[0].data().id.get() == 0,
          "valid reserved record image rejected");

    // Concurrent typed validation must never annotate the shared immutable image.
    constexpr std::size_t reader_count = 4;
    constexpr std::size_t reads_per_reader = 64;
    std::vector<std::future<void>> readers;
    for (std::size_t index = 0; index < reader_count; ++index) {
        readers.push_back(std::async(std::launch::async, [&] {
            for (std::size_t read = 0; read < reads_per_reader; ++read) {
                auto view = take(snapshot.array<vinc::Detection>(0));
                check(view[0].data().id.get() == 0, "concurrent snapshot read changed data");
                auto nested_view = take(nested_snapshot.array<vinc::MessageArray<vinc::Detection>>(0));
                check(nested_view[0][0].data().position.data().x.get() == 0,
                      "concurrent nested snapshot read changed data");
            }
        }));
    }
    for (auto& reader : readers) {
        reader.get();
    }
    check(copy_bytes(snapshot) == original, "typed validation changed immutable bytes");
}

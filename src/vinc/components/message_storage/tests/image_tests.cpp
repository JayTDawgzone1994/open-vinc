// Generic recording validates storage structure independently of generated field schemas.
#include <vinc/message/detail/framework_access.hpp>
#include <vinc/message/detail/image_format.hpp>
#include <vinc_messages/all_messages.hpp>
#include <algorithm>
#include <future>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
using Framework = vinc::message::detail::FrameworkAccess;
using vinc::message::ErrorCode;
using vinc::message::ErrorOperation;
namespace format = vinc::message::detail::format;
constexpr std::size_t fixture_bytes = 64 * 1024;

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

template <class T> T take(vinc::Result<T> result,
                         const std::source_location location = std::source_location::current()) {
    if (!result) {
        std::cerr << location.file_name() << ':' << location.line() << " code=" << result.error().code
                  << " operation=" << result.error().operation << '\n';
    }
    check(bool(result), "generic image fixture failed");
    return std::move(*result);
}

void ok(vinc::Result<void> result) {
    check(bool(result), "generic image mutation failed");
}

std::vector<std::byte> copy(const vinc::message::Snapshot& image) {
    return {image.bytes().begin(), image.bytes().end()};
}

// Decode fixture offsets byte-wise, independently of the production codec.
std::uint64_t read(const std::vector<std::byte>& bytes, std::size_t offset) {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        result |= std::uint64_t(std::to_integer<unsigned>(bytes.at(offset + index))) << (index * 8);
    }
    return result;
}

void write(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value, std::size_t width = 8) {
    for (std::size_t index = 0; index < width; ++index) {
        bytes.at(offset + index) = std::byte((value >> (index * 8)) & 255);
    }
}

std::size_t descriptor(const std::vector<std::byte>& bytes, std::uint64_t id) {
    return static_cast<std::size_t>(read(bytes, format::header::descriptor_table) + id * format::descriptor::size);
}

template <class T> void compare(const T& message) {
    const auto expected = take(message.snapshot());
    auto bytes = copy(expected);
    const auto* allocation = bytes.data();
    auto image = take(Framework::adopt_image(std::move(bytes), T::schema().hash, fixture_bytes));
    check(image.bytes().data() == allocation, "adoption copied the payload");
    auto canonical = take(Framework::canonical_image(image, expected.bytes().size()));
    check(copy(canonical) == copy(expected), "canonical image differs from typed snapshot");
    auto decoded = take(T::from_bytes(canonical.bytes()));
    check(copy(take(decoded->snapshot())) == copy(expected), "typed round trip changed canonical image");

    auto limited = Framework::canonical_image(image, expected.bytes().size() - 1);
    check(!limited && limited.error().is(ErrorCode::CapacityExceeded) &&
              limited.error().is_operation(ErrorOperation::Snapshot), "canonical limit lost error contract");
    check(image.bytes().data() == allocation && copy(image) == copy(expected), "failure changed source");
}

void reserved_image() {
    auto backing = std::make_shared<std::vector<std::byte>>(fixture_bytes);
    auto loan = take(Framework::borrow_message<vinc::Frame>(*backing, backing, fixture_bytes));
    loan->data().sequence = 73;
    ok(loan->data().payload.resize(3));
    loan->data().payload[0] = 17;
    ok(loan->data().payload.reserve(40));
    ok(loan->data().detections.resize(1));
    loan->data().detections[0].data().id = 99;
    ok(loan->data().detections[0].data().label.assign("camera"));
    ok(loan->data().groups.resize(1));
    ok(loan->data().groups[0].resize(1));
    ok(loan->data().groups[0][0].data().extra.resize(2));
    loan->data().groups[0][0].data().extra[1] = 42;
    ok(loan->data().labels.resize(2));
    ok(loan->data().labels[0].assign("nested text"));
    ok(loan->data().flags.resize(2));
    loan->data().flags[1] = true;
    const auto expected = take(loan->snapshot());
    auto sealed = take(Framework::seal(*loan));
    auto borrowed = Framework::canonical_image(sealed, fixture_bytes);
    check(!borrowed && borrowed.error().is(ErrorCode::InvalidArgument), "accepted a shared borrowed snapshot");
    auto bytes = copy(sealed);
    // Existing layout rules require unused capacity and padding to be zero.
    // Recording must retain that check, not silently sanitize invalid input.
    std::vector<bool> used(bytes.size(), false);
    const auto payload_start = read(bytes, format::header::payload);
    std::fill(used.begin(), used.begin() + static_cast<std::size_t>(payload_start), true);
    const auto count = read(bytes, format::header::node_count);
    for (std::uint64_t id = 0; id < count; ++id) {
        const auto entry = descriptor(bytes, id);
        const auto offset = read(bytes, entry);
        const auto size = read(bytes, entry + format::descriptor::element_count);
        // All scalar leaves here have widths 1, 4 or 8; other nodes store u64 IDs.
        const auto scalar = std::to_integer<unsigned>(bytes.at(entry + format::descriptor::scalar));
        const auto depth = std::to_integer<unsigned>(bytes.at(entry + format::descriptor::depth));
        std::size_t width = 8;
        if (depth == 1 && (scalar == 1 || scalar == 11)) {
            width = 1;
        } else if (depth == 1 && (scalar == 5 || scalar == 9)) {
            width = 4;
        }
        for (std::uint64_t index = 0; index < size * width; ++index) {
            used.at(static_cast<std::size_t>(offset + index)) = true;
        }
    }
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (!used[index]) {
            bytes[index] = std::byte{0xa5};
        }
    }
    auto dirty = Framework::adopt_image(std::move(bytes), vinc::Frame::schema().hash, fixture_bytes);
    check(!dirty && dirty.error().is(ErrorCode::InvalidData), "nonzero unused storage accepted");
    auto image = take(Framework::adopt_image(copy(sealed), vinc::Frame::schema().hash, fixture_bytes));
    sealed = {};
    loan.reset();
    backing.reset();
    const auto before = copy(image);
    auto canonical = take(Framework::canonical_image(image, fixture_bytes));
    check(copy(canonical) == copy(expected), "reserved capacity or padding leaked into canonical output");
    check(canonical.bytes().size() < image.bytes().size(), "reserved image did not compact");
    auto decoded = take(vinc::Frame::from_bytes(canonical.bytes()));
    check(decoded->data().sequence.get() == 73 && decoded->data().detections[0].data().id.get() == 99 &&
              decoded->data().groups[0][0].data().extra[1].get() == 42,
          "generic record lost nested values");
    auto reader = std::async(std::launch::async, [&] {
        return take(Framework::canonical_image(image, fixture_bytes));
    });
    check(copy(take(Framework::canonical_image(image, fixture_bytes))) == copy(reader.get()),
          "concurrent canonicalization changed output");
    check(copy(image) == before, "canonicalization modified immutable source");
}

void invalid_images() {
    vinc::Detection value;
    value.data().id = 19;
    const auto original = copy(take(value.snapshot()));
    const auto hash = vinc::Detection::schema().hash;
    const auto root = read(original, format::header::size);
    const auto root_descriptor = descriptor(original, root);
    const auto root_payload = static_cast<std::size_t>(read(original, root_descriptor));
    const auto id = read(original, root_payload);
    auto reject = [&](std::vector<std::byte> bytes, ErrorCode code = ErrorCode::InvalidData) {
        auto result = Framework::adopt_image(std::move(bytes), hash, fixture_bytes);
        check(!result && result.error().is(code) && result.error().is_operation(ErrorOperation::ValidateImage),
              "invalid generic image accepted or diagnostic lost");
    };
    auto damaged = original;
    damaged.pop_back();
    reject(std::move(damaged));
    damaged = original;
    write(damaged, format::header::version, format::version + 1, 4);
    reject(std::move(damaged));
    damaged = original;
    write(damaged, descriptor(original, id), UINT64_MAX);
    reject(std::move(damaged));
    damaged = original;
    write(damaged, format::header::flags, format::reserved_capacity_flag, 4);
    write(damaged, descriptor(original, id) + format::descriptor::capacity, UINT64_MAX);
    reject(std::move(damaged));
    damaged = original;
    write(damaged, format::header::root_count, UINT64_MAX);
    reject(std::move(damaged));
    damaged = original;
    write(damaged, root_payload, root);
    reject(std::move(damaged));
    damaged = original;
    write(damaged, root_payload + 8, id);
    reject(std::move(damaged));
    damaged = original;
    write(damaged, root_descriptor + format::descriptor::schema_hash, hash ^ 1);
    reject(std::move(damaged), ErrorCode::TypeMismatch);
    reject({});

    auto limited = Framework::adopt_image(original, hash, original.size() - 1);
    check(!limited && limited.error().is(ErrorCode::CapacityExceeded), "input byte limit ignored");
    auto invalid_hash = Framework::adopt_image(original, 0, fixture_bytes);
    check(!invalid_hash && invalid_hash.error().is(ErrorCode::InvalidArgument), "zero expected hash accepted");
    auto closed = Framework::canonical_image({}, fixture_bytes);
    check(!closed && closed.error().is(ErrorCode::Closed), "empty snapshot accepted");

    vinc::message::Message roots;
    auto first = take(roots.add_array<std::uint32_t>());
    ok(first.push_back(1));
    reject(copy(take(roots.snapshot())), ErrorCode::TypeMismatch);
    auto second = take(roots.add_array<std::uint32_t>());
    ok(second.push_back(2));
    reject(copy(take(roots.snapshot())));
    vinc::message::Message no_roots;
    reject(copy(take(no_roots.snapshot())));

    // Same-width uint32 -> float is structurally valid. Recording must succeed,
    // while typed decoding continues to reject the incompatible field schema.
    damaged = original;
    write(damaged, descriptor(original, id) + format::descriptor::scalar, 9, 4);
    auto unknown = take(Framework::adopt_image(std::move(damaged), hash, fixture_bytes));
    auto canonical = take(Framework::canonical_image(unknown, fixture_bytes));
    auto typed = vinc::Detection::from_bytes(canonical.bytes());
    check(!typed && typed.error().is(ErrorCode::TypeMismatch), "typed field validation was relaxed");

    // The generic path requires only the declared hash, not a registered C++ type.
    constexpr std::uint64_t foreign_hash = 0x123456789abcdefULL;
    damaged = original;
    write(damaged, root_descriptor + format::descriptor::schema_hash, foreign_hash);
    auto foreign = take(Framework::adopt_image(std::move(damaged), foreign_hash, fixture_bytes));
    check(bool(Framework::canonical_image(foreign, fixture_bytes)), "unknown record hash could not be saved");
}
}

void generic_image_tests() {
    compare(vinc::Empty{});
    vinc::Types scalars;
    scalars.data().a = -7;
    scalars.data().b = 231;
    scalars.data().c = -1000;
    scalars.data().d = 42000;
    scalars.data().e = -100000;
    scalars.data().f = 300000;
    scalars.data().g = -9000000000LL;
    scalars.data().h = 18000000000ULL;
    scalars.data().yes = true;
    scalars.data().precise = 0.125;
    compare(scalars);
    compare(vinc::Frame{});
    reserved_image();
    invalid_images();
    std::cout << "PASS: owned generic images, canonical parity, limits, invalid input and typed schema boundary\n";
}

// Verify generated classes preserve value copies, bound field access and nested records.
// Fixed-loan invalidation and malformed-schema tests exercise the same storage path used by Message Bus.

#include <vinc_messages/all_messages.hpp>
#include <vinc/message/detail/framework_access.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/process.hpp>
#include <iostream>
#include "contract_probe.hpp"
#include <random>
#include <cmath>
#include <algorithm>
#include <limits>

using namespace vinc;
using namespace vinc::message;
using vinc::Result;
using Access = vinc::message::detail::FrameworkAccess;
namespace os = vinc::osal;
using namespace std::chrono_literals;

namespace {
constexpr std::size_t mebibyte = 1024 * 1024;
constexpr std::size_t process_payload_bytes = 2 * mebibyte;
constexpr std::size_t process_mapping_bytes = 4 * mebibyte;
constexpr std::size_t payload_sample_index = 123456;
constexpr auto child_cleanup_timeout = 5s;
constexpr auto child_exit_timeout = 15s;
}

// Numeric addresses only verify relocation; they never cross back into pointers.
// Supported targets use flat addresses representable by uintptr_t, with no alignment change.
std::uintptr_t mapping_address(std::span<const std::byte> bytes) {
    return reinterpret_cast<std::uintptr_t>(bytes.data());
}

void require(bool ok, const char* text) {
    if (!ok) {
        throw std::runtime_error(text);
    }
}

template <class T, class E> T take(std::expected<T, E> value) {
    if (!value) {
        throw std::runtime_error("operation error " + std::to_string(static_cast<int>(value.error().code)));
    }

    return std::move(*value);
}

template <class E> void check(std::expected<void, E> value) {
    if (!value) {
        throw std::runtime_error("operation error " + std::to_string(static_cast<int>(value.error().code)));
    }
}

template <class F> void closed(F&& action) {
    contract_test::expect(std::forward<F>(action));
}

void fill(Frame& frame) {
    frame.data().sequence = 42;
    check(frame.data().detections.resize(2));
    auto detection = frame.data().detections[0];
    detection.data().id = 100;
    detection.data().confidence = 0.75f;
    detection.data().position.data().x = 1.25f;
    check(detection.data().label.assign("person"));
    check(detection.data().extra.copy_from({1, 2, 3}));
    check(frame.data().groups.resize(2));
    check(frame.data().groups[1].push_back(detection));
    check(frame.data().flags.copy_from({true, false, true}));
    check(frame.data().labels.resize(2));
    check(frame.data().labels[0].assign("front"));
    check(frame.data().labels[1].assign("rear"));
}

void inspect(const Frame& frame) {
    require(frame.data().sequence.get() == 42, "sequence");
    const auto detection = frame.data().detections[0];
    require(detection.data().id.get() == 100 && detection.data().confidence.get() == 0.75f, "record scalar");
    require(detection.data().position.data().x.get() == 1.25f, "nested record");
    require(detection.data().label.str() == "person" && detection.data().extra[2] == 3,
            "record variable fields");
    require(frame.data().groups[1][0].data().id.get() == 100, "nested record arrays");
    require(frame.data().flags[0] && !frame.data().flags[1], "bool array");
    require(frame.data().labels[1].str() == "rear", "string array");
}

// Distinguish auto value copies from auto& aliases, especially for nested record fields.
// Relocation must preserve a live binding while removal must invalidate it.
void value_tests() {
    auto frame = vinc::make_message<Frame>();
    fill(*frame);
    inspect(*frame);

    // Ordinary auto copies detach; assignments to those copies cannot mutate frame.
    auto copied = frame->data().detections;
    copied[0].data().id = 999;
    require(frame->data().detections[0].data().id.get() == 100, "auto array copy aliased message");
    auto copied_record = take(frame->data().detections.at(0));
    copied_record.data().position.data().x = 9;
    require(frame->data().detections[0].data().position.data().x.get() == 1.25f, "auto record copy aliased");
    auto copied_scalar = frame->data().sequence;
    copied_scalar = 777;
    require(frame->data().sequence.get() == 42, "auto scalar copy aliased");
    Frame other = *frame;
    other.data().detections[0].data().id = 5;
    for (auto detection : other.data().detections) {
        check(detection.data().extra.resize(50));
    }

    require(other.data().detections[1].data().extra.size() == 50,
            "nested growth invalidated outer iteration");
    auto records = take(other.data().detections.to_vector());
    check(other.data().detections.copy_from(records));
    auto record_image = take(other.data().detections.snapshot());
    (void)record_image;
    vinc::MessageArray<Detection> detached;
    check(detached.copy_from(records));
    auto detached_image = take(detached.snapshot());
    auto detached_view = take(detached_image.array<Detection>(0));
    require(detached_view[0].data().id.get() == 5, "record array snapshot view");
    inspect(*frame);

    // A copied Data_t can initialize another independently owned message.
    auto data = frame->data();
    data.detections[0].data().id = 17;
    inspect(*frame);
    Frame restored(data);
    require(restored.data().detections[0].data().id.get() == 17, "Data_t construction");

    // Reconstruct a typed facade only after validating the encoded schema.
    auto image = take(frame->snapshot());
    auto received = take(Frame::from_bytes(image.bytes()));
    inspect(*received);
    auto mismatch = Detection::from_bytes(image.bytes());
    require(!mismatch && mismatch.error().is(vinc::message::ErrorCode::TypeMismatch),
            "wrong schema accepted");

    // The append API supports consecutive images with a caller-owned offset.
    std::vector<std::uint8_t> serialized;
    check(frame->serialize(serialized));
    Frame decoded;
    BaseMessage& decoded_base = decoded;
    check(decoded_base.deserialize(serialized));
    inspect(decoded);
    auto truncated = serialized;
    truncated.pop_back();
    auto rejected = decoded.deserialize(truncated);
    require(!rejected && rejected.error().is(ErrorCode::InvalidData), "truncated input accepted");
    inspect(decoded);
    check(frame->serialize(serialized));
    std::size_t offset = 0;
    check(decoded_base.deserialize(serialized, offset));
    require(offset == image.bytes().size(), "offset deserialize first");
    check(decoded_base.deserialize(serialized, offset));
    require(offset == serialized.size(), "offset deserialize second");
    rejected = decoded.deserialize(serialized);
    require(!rejected && rejected.error().is(ErrorCode::InvalidData), "trailing bytes accepted");
    inspect(decoded);
    require(frame->to_string().find("person") != std::string::npos, "nested to_string");

    // Empty and canonical scalar schemas exercise the generated codecs.
    Empty empty;
    require(take(Empty::from_bytes(take(empty.snapshot()).bytes()))->to_string() == "Empty {}",
            "empty message");
    Types types;
    types.data().a = INT8_MIN;
    types.data().b = UINT8_MAX;
    types.data().c = INT16_MIN;
    types.data().d = UINT16_MAX;
    types.data().e = INT32_MIN;
    types.data().f = UINT32_MAX;
    types.data().g = INT64_MIN;
    types.data().h = UINT64_MAX;
    types.data().yes = true;
    types.data().precise = -0.0;
    auto typed = take(Types::from_bytes(take(types.snapshot()).bytes()));
    require(typed->data().a.get() == INT8_MIN && typed->data().h.get() == UINT64_MAX &&
                std::signbit(typed->data().precise.get()),
            "canonical scalar codecs");
    require(std::isnan(Types::not_a_number) && Types::minimum == INT64_MIN &&
                Types::greeting == "hello // world",
            "constants");

    // Arbitrary local C++ values do not become publishable message records.
    vinc::MessageArray<std::unique_ptr<int>> arbitrary;
    check(arbitrary.push_back(std::make_unique<int>(8)));
    require(*arbitrary[0] == 8, "arbitrary move-only local object");
    vinc::MessageArray<std::string> strings;
    check(strings.emplace_back("hello"));
    auto copy_strings = strings;
    copy_strings[0] = "changed";
    require(strings[0] == "hello", "ordinary object value semantics");
    std::cout << "PASS: generated classes, factory, nested fields, value copies, strings/bool, canonical "
                 "types and decoding\n";
}

// Check failure atomicity through the public interface, including nonzero cursors
// and fixed-capacity destinations. Wire offsets are independent test fixtures.
void deserialize_tests() {
    Frame source;
    fill(source);
    std::vector<std::uint8_t> encoded;
    check(source.serialize(encoded));
    Frame destination;
    fill(destination);
    destination.data().sequence = 99;
    std::vector<std::uint8_t> original;
    check(destination.serialize(original));

    auto reject = [&](const std::vector<std::uint8_t>& input, std::size_t initial_offset,
                      ErrorCode code, ErrorOperation operation) {
        auto offset = initial_offset;
        auto result = destination.deserialize(input, offset);
        require(!result && result.error().is(code) && result.error().is_operation(operation),
                "deserialize lost error classification or operation");
        require(result.error().causes().empty(), "same-component decode added a cause");
        require(offset == initial_offset, "failed decode advanced cursor");
        std::vector<std::uint8_t> after;
        check(destination.serialize(after));
        require(after == original, "failed decode changed destination");
    };

    reject(encoded, encoded.size() + 1, ErrorCode::InvalidArgument, ErrorOperation::Deserialize);
    reject(encoded, (std::numeric_limits<std::size_t>::max)(), ErrorCode::InvalidArgument,
           ErrorOperation::Deserialize);
    reject(encoded, encoded.size(), ErrorCode::InvalidData, ErrorOperation::Deserialize);
    reject({}, 0, ErrorCode::InvalidData, ErrorOperation::Deserialize);

    // Prefix bytes exercise decoding an image from a nonzero offset.
    constexpr std::size_t prefix_bytes = 3;
    std::vector<std::uint8_t> prefixed(prefix_bytes, 0);
    check(source.serialize(prefixed));
    auto truncated = prefixed;
    truncated.pop_back();
    reject(truncated, prefix_bytes, ErrorCode::InvalidData, ErrorOperation::Deserialize);
    constexpr std::size_t header_bytes = 64;
    for (std::size_t length = 0; length < header_bytes; ++length) {
        const std::vector<std::uint8_t> partial(encoded.begin(), encoded.begin() + length);
        reject(partial, 0, ErrorCode::InvalidData, ErrorOperation::Deserialize);
    }

    constexpr std::size_t total_bytes_offset = 16;
    constexpr std::size_t length_bytes = 8;
    auto bad_length = encoded;
    std::fill_n(bad_length.begin() + total_bytes_offset, length_bytes, std::uint8_t{0});
    reject(bad_length, 0, ErrorCode::InvalidData, ErrorOperation::Deserialize);
    std::fill_n(bad_length.begin() + total_bytes_offset, length_bytes, UINT8_MAX);
    reject(bad_length, 0, ErrorCode::InvalidData, ErrorOperation::Deserialize);
    auto bad_magic = encoded;
    bad_magic[0] = 0;
    reject(bad_magic, 0, ErrorCode::InvalidData, ErrorOperation::ValidateImage);
    Detection other_type;
    std::vector<std::uint8_t> wrong_schema;
    check(other_type.serialize(wrong_schema));
    reject(wrong_schema, 0, ErrorCode::TypeMismatch, ErrorOperation::ValidateImage);

    // A successful replacement keeps the fixed binding and its budget; a later
    // oversized image must preserve every byte, existing alias and cursor.
    constexpr std::size_t loan_bytes = 8192;
    auto backing = std::make_shared<std::vector<std::byte>>(loan_bytes);
    auto loan = take(Access::borrow_message<Frame>(*backing, backing, loan_bytes));
    auto& sequence = loan->data().sequence;
    std::size_t offset = prefix_bytes;
    check(loan->deserialize(prefixed, offset));
    require(offset == prefixed.size() && sequence.get() == 42, "decode lost fixed binding");
    require(take(loan->usage()).limit_bytes == loan_bytes, "decode changed fixed budget");
    sequence = 99;
    const auto before = *backing;
    check(source.data().payload.resize(loan_bytes));
    std::vector<std::uint8_t> oversized(prefix_bytes, 0);
    check(source.serialize(oversized));
    offset = prefix_bytes;
    auto result = loan->deserialize(oversized, offset);
    require(!result && result.error().is(ErrorCode::CapacityExceeded) &&
                result.error().is_operation(ErrorOperation::Assign), "decode hid capacity failure");
    require(*backing == before && offset == prefix_bytes && sequence.get() == 99,
            "failed fixed decode changed bytes, cursor or alias");
    check(loan->deserialize(encoded));
    inspect(*loan);
    Access::revoke(*loan);
    closed([&] {
        (void)loan->deserialize(encoded);
    });
}

void loan_tests() {
    constexpr std::size_t capacity = mebibyte;
    auto backing = std::make_shared<std::vector<std::byte>>(capacity);
    auto loan = take(Access::borrow_message<Frame>(*backing, backing, capacity));
    fill(*loan);
    auto& array_ref = loan->data().detections;
    auto array_copy = array_ref;
    auto moved_field = std::move(loan->data().detections);
    moved_field[0].data().id = 500;
    inspect(*loan);
    auto& scalar_ref = loan->data().sequence;
    auto record_proxy = loan->data().detections[0];
    auto record_fields = record_proxy.data();
    const auto before = *backing;
    auto failed = loan->data().payload.resize(capacity * 2);
    require(!failed && failed.error().is(vinc::message::ErrorCode::CapacityExceeded) && *backing == before,
            "loan overflow changed bytes");
    auto source = vinc::make_message<Frame>();
    fill(*source);
    check(source->data().detections[0].data().extra.resize(capacity * 2));
    failed = array_ref.assign(source->data().detections);
    require(!failed && failed.error().is(vinc::message::ErrorCode::CapacityExceeded) && *backing == before,
            "array assignment bypassed loan budget");
    auto imported = take(source->data().detections.to_vector());
    failed = array_ref.copy_from(imported);
    require(!failed && failed.error().is(vinc::message::ErrorCode::CapacityExceeded) && *backing == before,
            "batch record import bypassed loan budget");

    // Existing aliases must become unusable without moving the sealed payload.
    auto image = take(Access::seal(*loan));
    require(*backing == before && image.bytes().data() == backing->data(), "seal changed/copied payload");
    closed([&] {
        scalar_ref = 7;
    });
    closed([&] {
        array_ref[0].data().id = 7;
    });
    closed([&] {
        (void)record_proxy.data();
    });
    closed([&] {
        record_fields.id = 7;
    });
    array_copy[0].data().id = 900;
    require(array_copy[0].data().id.get() == 900, "detached copy lost after commit");
    auto received = take(Frame::from_bytes(image.bytes()));
    inspect(*received);

    // Reuse backing only after dropping the prior publication lease.
    image = Snapshot{}; // release the old slot lease before reusing this test backing
    auto local = vinc::make_message<Frame>();
    fill(*local);
    auto pushed = take(Access::push(*local, *backing, backing));
    require(pushed.bytes().data() == backing->data(), "local push destination");
    inspect(*local);
    auto pushed_copy = take(Frame::from_bytes(pushed.bytes()));
    inspect(*pushed_copy);
    auto too_small = std::make_shared<std::vector<std::byte>>(64, std::byte{99});
    auto rejected = Access::push(*local, *too_small, too_small);
    require(!rejected && rejected.error().is(vinc::message::ErrorCode::CapacityExceeded) &&
                (*too_small)[0] == std::byte{99},
            "rejected push wrote destination");
    inspect(*local);
    std::cout << "PASS: shared budget, assignment failure atomicity, unchanged seal bytes, detached copy and "
                 "local push\n";
}

std::uint64_t get64(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        result |= std::uint64_t(std::to_integer<unsigned>(bytes[offset + i])) << (i * 8);
    }

    return result;
}

void put64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        bytes.at(offset + i) = static_cast<std::byte>((value >> (i * 8)) & 255);
    }
}

// Literal offsets below intentionally remain independent of the production format constants.
// These fixtures catch accidental wire-layout changes rather than mirroring the encoder.
void malformed_tests() {
    auto frame = vinc::make_message<Frame>();
    fill(*frame);

    // Reconstruct a typed facade only after validating the encoded schema.
    auto image = take(frame->snapshot());
    std::vector<std::byte> original(image.bytes().begin(), image.bytes().end());
    const auto table = static_cast<std::size_t>(get64(original, 40));
    const auto root = static_cast<std::size_t>(get64(original, 64));
    const auto root_descriptor = table + root * 48;
    const auto root_payload = static_cast<std::size_t>(get64(original, root_descriptor));
    auto wrong_hash = original;
    put64(wrong_hash, root_descriptor + 32, 123);
    require(!Frame::from_bytes(wrong_hash), "wrong root schema accepted");
    auto wrong_scalar = original;
    const auto sequence_id = static_cast<std::size_t>(get64(original, root_payload));
    wrong_scalar.at(table + sequence_id * 48 + 16) = std::byte{8}; // uint64 -> int64, same width
    require(!Frame::from_bytes(wrong_scalar), "schema field type not validated");
    auto cycle = original;
    put64(cycle, root_payload, root);
    require(!Frame::from_bytes(cycle), "record ownership cycle accepted");
    auto duplicate = original;
    put64(duplicate, root_payload + 8, sequence_id);
    require(!Frame::from_bytes(duplicate), "shared ownership accepted");
    auto huge = original;
    put64(huge, table + sequence_id * 48 + 24, UINT64_MAX);
    require(!Frame::from_bytes(huge), "overflow capacity accepted");
    for (std::size_t size : {std::size_t{0}, std::size_t{63}, original.size() - 1}) {
        require(!Frame::from_bytes(std::span(original).first(size)), "truncated image accepted");
    }

    std::cout << "PASS: generated schema validation, cycles, duplicate ownership, bounds and overflow\n";
}

struct ShmCleanup {
    std::string name;

    ~ShmCleanup() {
#ifndef _WIN32
        (void)os::SharedMemory::unlink_name(name);
#endif
    }
};

struct ChildCleanup {
    os::Process* child;

    ~ChildCleanup() {
        (void)child->terminate();
        (void)child->wait_until(std::chrono::steady_clock::now() + child_cleanup_timeout);
    }
};

int receive_helper(const std::string& name, std::uint64_t length, std::uintptr_t sender_address) {
    auto shm = take(os::SharedMemory::open(name, os::SharedMemoryAccess::ReadOnly));
    auto mapping = take(shm.map(length, os::SharedMemoryAccess::ReadOnly));
    auto other = take(shm.map(length, os::SharedMemoryAccess::ReadOnly));
    if (mapping_address(mapping.bytes()) == sender_address) {
        mapping = std::move(other);
    }

    require(mapping_address(mapping.bytes()) != sender_address, "same-address test setup");
    auto received = take(Frame::from_bytes(mapping.bytes()));
    check(mapping.unmap());
    check(other.unmap());
    check(shm.close());
    inspect(*received);
    require(received->data().payload.size() == process_payload_bytes &&
                take(received->data().payload.at(payload_sample_index)) == 77,
            "large receive copy");
    received->data().sequence = 999; // received message is independently mutable
    check(received->data().detections[0].data().extra.push_back(4));
    return 0;
}

void process_tests(const std::filesystem::path& executable) {
    constexpr std::uint64_t capacity = process_mapping_bytes;
    ShmCleanup cleanup{
        "r" + std::to_string((std::uint64_t(std::random_device{}()) << 32) | std::random_device{}())};
    auto shm = take(os::SharedMemory::create(cleanup.name, capacity));
    auto mapping =
        std::make_shared<os::SharedMemoryMapping>(take(shm.map(capacity, os::SharedMemoryAccess::ReadWrite)));
    auto loan = take(Access::borrow_message<Frame>(take(mapping->writable_bytes()), mapping, capacity));
    fill(*loan);
    check(loan->data().payload.resize(process_payload_bytes));
    loan->data().payload[payload_sample_index] = 77;

    // Existing aliases must become unusable without moving the sealed payload.
    auto image = take(Access::seal(*loan));
    auto array_copy = take(Frame::from_bytes(image.bytes()))->data().detections;
    mapping.reset();
    check(shm.close()); // image owns the mapping lifetime
    auto service = take(os::ProcessService::create());
    os::SpawnOptions options;
    options.executable = executable;
    options.arguments = {"--receive", cleanup.name, std::to_string(image.bytes().size()),
                         std::to_string(mapping_address(image.bytes()))};
    auto child = take(service.spawn(options));
    ChildCleanup guard{&child};
    auto status = take(child.wait_until(std::chrono::steady_clock::now() + child_exit_timeout));
    require(status.kind == os::ExitKind::Code && status.value == 0, "receiver child failed");
    check(service.forget(child.id()));
    require(array_copy[0].data().id.get() == 100, "array copy lost with source facade");
    std::cout << "PASS: generated nested message across OSAL processes; one receive copy survives unmap\n";
}

int main(int argc, char** argv) {
    const bool probe = contract_test::initialize(argc, argv);
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--receive") {
            return receive_helper(argv[2], std::stoull(argv[3]), std::stoull(argv[4]));
        }

        require(probe || argc == 2, "expected executable path");
        value_tests();
        deserialize_tests();
        loan_tests();
        malformed_tests();
        process_tests(std::filesystem::absolute(argv[1]));
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}

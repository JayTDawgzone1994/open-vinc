// Numeric error transport is independent of native C++ layout and local chain capacity.
#include <vinc/message_bus/detail/protocol.hpp>
#include <vinc/message_bus/detail/bus_manager.hpp>
#include <vinc/message_bus/detail/error_catalog.hpp>
#include <vinc/osal/error.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vinc;
namespace bus = vinc::message_bus;
namespace protocol = vinc::message_bus::detail;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_roundtrip() {
    const auto native = NativeError{static_cast<NativeErrorSource>(999), -123456789};
    const auto leaf = Error::from_raw(static_cast<ErrorDomain>(999), -123, -456, native);
    const auto channel = wrap_error(control::ErrorCode::Io, control::ErrorOperation::Receive, leaf);
    const auto failure =
        wrap_error(bus::ErrorCode::Io, bus::ErrorOperation::Advertise, channel);
    auto decoded = protocol::decode_error(protocol::encode_error(failure));
    check(decoded && *decoded == failure, "error records survive numeric transport");
    auto response = protocol::decode_response(protocol::encode_failure(failure));
    check(response && response->failure && *response->failure == failure, "failed response owns the entire chain");
    const auto text = error_to_string(*response->failure);
    check(text.find("MessageBus") != std::string::npos &&
              text.find("ControlChannel") != std::string::npos &&
              text.find("-123456789") != std::string::npos,
          "static catalogs and foreign numeric diagnostics are available in receiver");
}

void test_validation() {
    const Error error{bus::ErrorCode::InvalidData, bus::ErrorOperation::Push};
    const auto valid = protocol::encode_error(error);
    for (std::size_t size = 0; size < valid.size(); ++size) {
        check(!protocol::decode_error(std::span<const std::byte>(valid).first(size)),
              "reject truncated payload");
    }
    auto malformed = valid;
    malformed[3] = std::byte{2};
    check(!protocol::decode_error(malformed), "reject unknown error wire version");
    malformed = valid;
    malformed[7] = std::byte{0};
    check(!protocol::decode_error(malformed), "reject empty chain");
    malformed = valid;
    malformed[7] = std::byte{17};
    check(!protocol::decode_error(malformed), "reject excessive count before allocation");
    malformed = valid;
    malformed[11] = std::byte{1};
    check(!protocol::decode_error(malformed), "truncation requires a retained cause");
    malformed = valid;
    malformed[11] = std::byte{2};
    check(!protocol::decode_error(malformed), "reject unknown flags");
    malformed = valid;
    malformed.push_back(std::byte{0});
    check(!protocol::decode_error(malformed), "reject trailing bytes");

    control::Bytes response;
    control::put_u32(response, 2);
    check(!protocol::decode_response(response), "reject unknown response tag");
    auto foreign = protocol::decode_response(protocol::encode_failure(Error{osal::ErrorCode::Timeout}));
    check(!foreign && foreign.error().is(bus::ErrorCode::Protocol),
          "Bus response cannot expose an unwrapped lower domain");
}

void test_catalog_and_boundaries() {
    for (const auto& entry : protocol::error_catalog.codes) {
        const auto error = Error::from_raw(ErrorDomain::MessageBus, entry.value, 0);
        check(error_to_string(error).contains(entry.text), "selected Bus error catalog");
    }
    for (const auto& entry : protocol::error_catalog.operations) {
        const auto error = Error::from_raw(ErrorDomain::MessageBus, 11, entry.value);
        check(error_to_string(error).contains(entry.text), "selected Bus operation catalog");
    }

    const Error reported{bus::ErrorCode::Protocol, bus::ErrorOperation::Advertise};
    auto response = protocol::decode_response(protocol::encode_failure(reported));
    check(response && response->failure == reported, "remote Protocol failure is a valid response");
    const auto unknown = Error::from_raw(ErrorDomain::MessageBus, -70001, -70002);
    response = protocol::decode_response(protocol::encode_failure(unknown));
    check(response && response->failure == unknown, "unknown signed Bus identities retained");
    check(error_to_string(*response->failure).contains("-70001"), "unknown error remains numeric");

    control::Bytes success;
    control::put_u32(success, protocol::result_success);
    success.push_back(std::byte{42});
    auto decoded = protocol::decode_response(success);
    check(decoded && !decoded->failure && decoded->body.size() == 1 &&
              decoded->body.front() == std::byte{42}, "success body preserved");

    auto manager = protocol::BusManager::create();
    check(bool(manager), "create admission fixture");
    for (const auto operation : {1, 2, 3, 4, 5, 6, 0x0301, static_cast<int>(protocol::ListTopicConnections) + 1}) {
        control::Frame frame{control::Kind::Request, protocol::message_bus_domain,
                             static_cast<std::uint16_t>(operation), 1};
        auto rejected = (*manager)->submit(1, "fixture", {}, std::move(frame));
        check(!rejected && rejected.error().is(bus::ErrorCode::Protocol),
              "old/unknown operations rejected before queueing a mutation");
    }

    const Error storage{message::ErrorCode::CapacityExceeded, message::ErrorOperation::Resize};
    const auto translated = protocol::storage_error(storage, bus::ErrorOperation::Borrow);
    check(translated.is(bus::ErrorCode::CapacityExceeded) && !translated.is(message::ErrorCode::CapacityExceeded),
          "storage category translated with domain");
    check(translated.causes().size() == 1 && translated.causes().front() == storage.record(),
          "storage cause retained");
}

void test_local_capacity() {
    control::Bytes bytes;
    control::put_u32(bytes, protocol::error_wire_version);
    control::put_u32(bytes, static_cast<std::uint32_t>(protocol::error_wire_max_records));
    control::put_u32(bytes, 0);
    for (std::size_t index = 0; index < protocol::error_wire_max_records; ++index) {
        protocol::encode_error_record(
            bytes,
            Error::from_raw(ErrorDomain::MessageBus, static_cast<std::int32_t>(index + 1), 0).record());
    }
    auto decoded = protocol::decode_error(bytes);
    check(bool(decoded), "accept a sender with a different chain capacity");
    const auto retained = (std::min)(config::error_chain_max_depth, protocol::error_wire_max_records);
    check(decoded->causes().size() + 1 == retained && decoded->causes().back().code == 16,
          "keep deepest cause when receiver capacity is smaller");
    check(decoded->truncated() == (retained < protocol::error_wire_max_records), "report local truncation");
}
}

int main() {
    try {
        test_roundtrip();
        test_validation();
        test_local_capacity();
        test_catalog_and_boundaries();
        std::cout << "Message Bus error protocol passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

// Numeric error transport is independent of native C++ layout and local chain capacity.
#include <vinc/message_bus/detail/protocol.hpp>
#include <vinc/message_bus/detail/topology_protocol.hpp>
#include <vinc/message_bus/detail/bus_manager.hpp>
#include <vinc/message_bus/detail/error_catalog.hpp>
#include <vinc/message_bus/detail/runtime.hpp>
#include <vinc/osal/error.hpp>
#include <algorithm>
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

void test_receive_after_close() {
    const auto valid_notification = [] {
        control::Frame frame{control::Kind::Event, protocol::message_bus_domain,
                             protocol::NotificationRoutesChanged};
        control::put_u64(frame.payload, 1);
        control::put_u32(frame.payload, 0);
        control::Bytes record;
        protocol::append_topology_record(record, frame.operation, frame.payload);
        frame.operation = protocol::SyncPage;
        frame.payload = *protocol::encode_topology_page({{1, 1, 0}, 0, 1, protocol::TopologyMode::Delta, true, record});
        return frame;
    };
    unsigned closes = 0;
    protocol::ControlTransport transport;
    transport.send = [](control::Frame) -> Result<void> {
        return {};
    };
    transport.close = [&] {
        ++closes;
    };
    auto client = protocol::RuntimeAccess::create(transport);
    check(bool(client), "create receive classification fixture");
    check(bool(protocol::RuntimeAccess::receive(**client, valid_notification())),
          "late notification for a retired Publisher is valid while transport is open");
    protocol::RuntimeAccess::shutdown(**client);
    auto closed = protocol::RuntimeAccess::receive(**client, valid_notification());
    check(!closed && closed.error().is(bus::ErrorCode::Closed) &&
          closed.error().is_operation(bus::ErrorOperation::Receive),
          "closed transport is not malformed protocol");

    auto malformed_client = protocol::RuntimeAccess::create(transport);
    check(bool(malformed_client), "create malformed receive fixture");
    auto malformed = valid_notification();
    malformed.payload.pop_back();
    const auto prior_closes = closes;
    auto rejected = protocol::RuntimeAccess::receive(**malformed_client, std::move(malformed));
    check(!rejected && rejected.error().is(bus::ErrorCode::Protocol) &&
          rejected.error().is_operation(bus::ErrorOperation::Receive) && closes > prior_closes,
          "first malformed frame reports Protocol even though rejection closes the transport");
    auto later = protocol::RuntimeAccess::receive(**malformed_client, valid_notification());
    check(!later && later.error().is(bus::ErrorCode::Closed),
          "subsequent receive observes closure without relabeling the original Protocol error");
}

void test_topology_envelope() {
    const protocol::TopologyPageId id{17, 4, 2};
    control::Bytes records(protocol::topology_page_capacity, std::byte{17});
    protocol::TopologyPage page{id, 31, 33, protocol::TopologyMode::Delta, true, records};
    auto encoded = protocol::encode_topology_page(page);
    check(encoded && encoded->size() == control::max_payload, "full topology page fits the transport");
    auto decoded = protocol::decode_topology_page(*encoded);
    check(decoded && decoded->id == id && decoded->base == page.base && decoded->target == page.target &&
          decoded->mode == page.mode && decoded->final && std::ranges::equal(decoded->records, records),
          "topology header and body round trip");
    const std::span<const std::byte> wire(*encoded);
    for (std::size_t length = 0; length < protocol::topology_header_bytes; ++length) {
        check(!protocol::decode_topology_page(wire.first(length)), "truncated topology header");
    }
    encoded->push_back(std::byte{0});
    check(!protocol::decode_topology_page(*encoded), "oversized topology page");
    page.base = page.target;
    check(!protocol::encode_topology_page(page), "delta cannot roll a revision back");
    page = {{17, 5, 0}, 0, 33, protocol::TopologyMode::Snapshot, true, {}};
    encoded = protocol::encode_topology_page(page);
    check(encoded && protocol::decode_topology_page(*encoded), "empty snapshot can clear old relationships");
    encoded->back() = std::byte{2};
    check(!protocol::decode_topology_page(*encoded), "malformed final flag");

    for (auto receipt : {protocol::TopologyReceipt::Staged, protocol::TopologyReceipt::Applied,
                         protocol::TopologyReceipt::Resync}) {
        auto bytes = protocol::encode_topology_ack({id, receipt});
        check(bool(bytes), "encode receipt");
        auto ack = protocol::decode_topology_ack(*bytes);
        check(ack && ack->id == id && ack->receipt == receipt, "receipt identifies the exact page");
        bytes->back() = std::byte{0};
        check(!protocol::decode_topology_ack(*bytes), "unknown receipt");
        bytes->pop_back();
        check(!protocol::decode_topology_ack(*bytes), "truncated receipt");
    }
}

void test_topology_page_sequence() {
    using protocol::PageAction;
    const control::Bytes records{std::byte{1}};
    protocol::TopologyReceiveCursor cursor(17);
    protocol::TopologyPage page{{17, 1, 0}, 0, 12, protocol::TopologyMode::Snapshot, false, records};
    check(cursor.classify(page) == PageAction::Stage, "first snapshot page");
    cursor.accept(page);
    check(cursor.applied() == 0 && cursor.classify(page) == PageAction::RepeatReceipt,
          "staging or receiving a duplicate must not advance the applied revision");
    page.id.index = 2;
    check(cursor.classify(page) == PageAction::RequestSnapshot, "missing page requests a snapshot");
    page.id.index = 1;
    page.final = true;
    check(cursor.classify(page) == PageAction::Commit, "complete snapshot ready for atomic installation");
    cursor.accept(page);
    check(cursor.applied() == 12 && cursor.classify(page) == PageAction::RepeatReceipt,
          "duplicate final page never reapplies data");
    page = {{17, 2, 0}, 11, 13, protocol::TopologyMode::Delta, true, records};
    check(cursor.classify(page) == PageAction::RequestSnapshot, "reject delta based on stale registration state");
    page.base = 12;
    check(cursor.classify(page) == PageAction::Commit, "next change follows completed snapshot");
    cursor.accept(page);
    page.id.generation = 16;
    check(cursor.classify(page) == PageAction::IgnoreOld && cursor.applied() == 13, "ignore old Session generation");
}

void test_topology_source_group() {
    protocol::Descriptor source;
    source.id = 21;
    source.owner = 2;
    source.schema = 31;
    source.rank = source.id;
    source.slots = 2;
    source.readers = 128;
    source.bytes = 1024;
    source.control = "control";
    source.data = "data";
    source.counter = "counter";
    source.publisher = "camera";
    std::vector<protocol::TopologyReader> readers;
    for (std::uint32_t index = 0; index < source.readers; ++index) {
        readers.push_back({1000u + index, index, 2000u + index});
    }
    auto encoded = protocol::encode_source_group(source, readers);
    check(encoded && encoded->size() <= protocol::topology_page_capacity, "128 relationships fit in one group");
    const auto decoded = protocol::decode_source_group(*encoded);
    check(decoded && decoded->publisher.control == source.control && decoded->readers == readers,
          "grouped subscriptions retain independent indices and initial floors");
    encoded->pop_back();
    check(!protocol::decode_source_group(*encoded), "truncated source group");
    readers.back().subscription = readers.front().subscription;
    check(!protocol::encode_source_group(source, readers), "duplicate subscription in source group");
}

void test_topology_ack_sequence() {
    using protocol::AckAction;
    using protocol::TopologyReceipt;
    protocol::TopologySendCursor sender(4);
    check(sender.start(17) && sender.ready_to_send(), "first page awaits admission");
    const auto first = sender.id();
    check(!sender.acknowledge({first, TopologyReceipt::Staged}) && sender.ready_to_send(),
          "a page rejected by transport cannot be acknowledged");
    sender.sent(false);
    check(!sender.ready_to_send() && !sender.acknowledge({first, TopologyReceipt::Applied}),
          "one page outstanding and no early commit");
    auto result = sender.acknowledge({first, TopologyReceipt::Staged});
    check(result && *result == AckAction::NextPage && sender.applied() == 0, "stage ACK only admits next page");
    result = sender.acknowledge({first, TopologyReceipt::Staged});
    check(result && *result == AckAction::IgnoreOld && sender.ready_to_send(), "duplicate stage ACK");
    sender.sent(true);
    result = sender.acknowledge({sender.id(), TopologyReceipt::Resync});
    check(result && *result == AckAction::RestartSnapshot && sender.applied() == 0, "abandon incomplete transfer");
    check(bool(sender.start(18)), "replacement snapshot");
    sender.sent(true);
    auto stale = sender.id();
    --stale.generation;
    result = sender.acknowledge({stale, TopologyReceipt::Applied});
    check(result && *result == AckAction::IgnoreOld && sender.applied() == 0, "old generation cannot commit");
    result = sender.acknowledge({sender.id(), TopologyReceipt::Applied});
    check(result && *result == AckAction::Finished && sender.applied() == 18, "only final ACK advances revision");
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
    for (const auto operation : {1, 2, 3, 4, 5, 6, 0x0201, 0x0301, 0x0401, 0x0501, 0x0701, static_cast<int>(protocol::SyncAck) + 1}) {
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
        test_receive_after_close();
        test_validation();
        test_local_capacity();
        test_topology_envelope();
        test_topology_page_sequence();
        test_topology_source_group();
        test_topology_ack_sequence();
        test_catalog_and_boundaries();
        std::cout << "Message Bus error protocol passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

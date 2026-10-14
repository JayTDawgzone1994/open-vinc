#include "../../../../tests/support/manager.hpp"
#include <vinc/module/detail/instance.hpp>
#include <vinc/rpc/client.hpp>
#include <vinc/rpc/detail/storage.hpp>
#include <vinc_messages/Frame.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vinc;
using namespace vinc::rpc;

void check(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

template <class T> auto take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

struct Access : rpc::detail::Access {
    std::deque<control::Frame> input;
    std::vector<control::Frame> output;
    std::uint64_t id = 1;

    bool living() const noexcept override {
        return true;
    }

    Result<std::uint64_t> reserve_request() override {
        return id++;
    }

    Result<control::Bytes> exchange(control::Frame) override {
        control::Bytes bytes;
        control::put_u64(bytes, 1);
        return bytes;
    }

    Result<void> send(control::Frame frame) override {
        output.push_back(std::move(frame));
        return {};
    }

    std::deque<control::Frame> take() override {
        std::deque<control::Frame> batch;
        batch.swap(input);
        return batch;
    }
};

void error_protocol() {
    const auto root = Error::from_raw(static_cast<ErrorDomain>(777), -123, 432);
    const auto error = wrap_error(rpc::ErrorCode::StorageFailed, rpc::ErrorOperation::Call, root);
    auto encoded = rpc::detail::encode_failure(error);
    auto decoded = take(rpc::detail::decode_response(encoded));
    check(decoded.failure && *decoded.failure == error, "complete unknown cause preserved");
    for (std::size_t size = 0; size < encoded.size(); ++size) {
        check(!rpc::detail::decode_response(std::span(encoded).first(size)), "truncated envelope rejected");
    }

    encoded.push_back(std::byte{0});
    check(!rpc::detail::decode_response(encoded), "trailing error bytes rejected");
    encoded = rpc::detail::encode_failure(error);
    encoded[7] = std::byte{99};
    check(!rpc::detail::decode_response(encoded), "unknown wire version rejected");
}

void storage_and_batch() {
    auto instance = take(vinc::detail::Instance::create(test::instance_name()));
    ServiceOptions options{4096, 4096, 3};
    auto pool = take(rpc::detail::Pool::create(instance, options));
    const auto stride = take(rpc::detail::slot_bytes(options));
    Access access;
    rpc::Client client;
    rpc::detail::ClientAccess::bind(client, access);
    auto queue = [&](std::uint64_t token, std::uint64_t index) {
        rpc::detail::Transfer transfer{token,
                                       1,
                                       pool->name(),
                                       pool->extent(),
                                       index * stride,
                                       options.request_bytes,
                                       options.response_bytes,
                                       0};
        auto message = make_message<Frame>();
        message->data().sequence = token;
        auto image = take(message->snapshot());
        transfer.size = image.bytes().size();
        auto memory = take(rpc::detail::TransferMemory::open(transfer));
        take(memory.write(false, image.bytes()));
        auto bytes = rpc::detail::encode_transfer(transfer);
        bytes.insert(bytes.begin(), 4, std::byte{0});
        access.input.push_back(
            {control::Kind::Response, rpc::detail::domain, rpc::detail::Invoke, token, std::move(bytes)});
    };
    std::vector<std::uint64_t> seen;
    take((client.register_rpc<Frame, Frame>("echo", options, [&](const Frame& request, Frame& response) {
        const auto sequence = request.data().sequence.get();
        seen.push_back(sequence);
        if (sequence == 1) {
            queue(3, 2);
        }

        response.data().sequence = sequence;
        return RpcStatus::success();
    })));
    queue(1, 0);
    queue(2, 1);
    auto first = take(client.pump());
    check(first.requests == 2 && seen == std::vector<std::uint64_t>{1, 2}, "frozen RPC batch");
    take(client.pump());
    check(seen == std::vector<std::uint64_t>{1, 2, 3}, "new arrival waits for next pump");
    queue(1, 0);
    take(client.pump());
    check(seen.size() == 3, "duplicate invocation never executes twice");

    rpc::detail::Transfer transfer{9, 1, pool->name(), pool->extent(), 0, 4096, 4096, 0};
    auto memory = take(rpc::detail::TransferMemory::open(transfer));
    check(!memory.read(false, 20), "old slot generation rejected");
    control::Bytes oversized(4097);
    check(!memory.write(false, oversized), "write capacity enforced");
    // A complete publication word is insufficient: invalid message layouts must
    // fail before entering application code, while preserving a typed failure.
    control::Bytes malformed(64, std::byte{0xff});
    take(memory.write(false, malformed));
    transfer.size = malformed.size();
    auto invalid_image = rpc::detail::encode_transfer(transfer);
    invalid_image.insert(invalid_image.begin(), 4, std::byte{0});
    access.input.push_back({control::Kind::Response, rpc::detail::domain, rpc::detail::Invoke, transfer.token,
                            std::move(invalid_image)});
    take(client.pump());
    check(seen.size() == 3, "invalid layout never reaches handler");
    auto rejected = take(rpc::detail::decode_response(access.output.back().payload));
    check(rejected.failure && rejected.failure->is(rpc::ErrorCode::StorageFailed), "layout cause preserved");
    transfer.offset = UINT64_MAX;
    check(!rpc::detail::decode_transfer(rpc::detail::encode_transfer(transfer)),
          "overflowing descriptor rejected");
    check(!rpc::detail::pool_bytes({UINT64_MAX, 1, 1}), "capacity arithmetic rejects overflow");
}
}

int main() {
    try {
        error_protocol();
        storage_and_batch();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

// Numeric error transport is independent of native C++ layout and local chain capacity.
#include <vinc/module/detail/error_protocol.hpp>
#include <vinc/osal/error.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vinc;

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
        wrap_error(module_system::ErrorCode::ChannelFailed, module_system::ErrorOperation::Receive, channel);
    auto decoded = detail::decode_error(detail::encode_error(failure));
    check(decoded && *decoded == failure, "error records survive numeric transport");
    auto response = detail::decode_response(detail::encode_failure(failure));
    check(!response && response.error() == failure, "failed response owns the entire chain");
    const auto text = error_to_string(response.error());
    check(text.find("ModuleSystem") != std::string::npos &&
              text.find("ControlChannel") != std::string::npos &&
              text.find("-123456789") != std::string::npos,
          "static catalogs and foreign numeric diagnostics are available in receiver");
}

void test_validation() {
    const Error error{module_system::ErrorCode::HookFailed, module_system::ErrorOperation::StartHook};
    const auto valid = detail::encode_error(error);
    for (std::size_t size = 0; size < valid.size(); ++size) {
        check(!detail::decode_error(std::span<const std::byte>(valid).first(size)),
              "reject truncated payload");
    }
    auto malformed = valid;
    malformed[3] = std::byte{2};
    check(!detail::decode_error(malformed), "reject unknown error wire version");
    malformed = valid;
    malformed[7] = std::byte{0};
    check(!detail::decode_error(malformed), "reject empty chain");
    malformed = valid;
    malformed[7] = std::byte{17};
    check(!detail::decode_error(malformed), "reject excessive count before allocation");
    malformed = valid;
    malformed[11] = std::byte{1};
    check(!detail::decode_error(malformed), "truncation requires a retained cause");
    malformed = valid;
    malformed[11] = std::byte{2};
    check(!detail::decode_error(malformed), "reject unknown flags");
    malformed = valid;
    malformed.push_back(std::byte{0});
    check(!detail::decode_error(malformed), "reject trailing bytes");

    control::Bytes response;
    control::put_u32(response, 2);
    check(!detail::decode_response(response), "reject unknown response tag");
    auto foreign = detail::decode_response(detail::encode_failure(Error{osal::ErrorCode::Timeout}));
    check(!foreign && foreign.error().is(module_system::ErrorCode::Protocol),
          "module response cannot expose an unwrapped lower domain");
}

void test_local_capacity() {
    control::Bytes bytes;
    control::put_u32(bytes, detail::error_wire_version);
    control::put_u32(bytes, static_cast<std::uint32_t>(detail::error_wire_max_records));
    control::put_u32(bytes, 0);
    for (std::size_t index = 0; index < detail::error_wire_max_records; ++index) {
        detail::encode_error_record(
            bytes,
            Error::from_raw(ErrorDomain::ModuleSystem, static_cast<std::int32_t>(index + 1), 0).record());
    }
    auto decoded = detail::decode_error(bytes);
    check(bool(decoded), "accept a sender with a different chain capacity");
    const auto retained = (std::min)(config::error_chain_max_depth, detail::error_wire_max_records);
    check(decoded->causes().size() + 1 == retained && decoded->causes().back().code == 16,
          "keep deepest cause when receiver capacity is smaller");
    check(decoded->truncated() == (retained < detail::error_wire_max_records), "report local truncation");
}
}

int main() {
    try {
        test_roundtrip();
        test_validation();
        test_local_capacity();
        std::cout << "Module error protocol passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

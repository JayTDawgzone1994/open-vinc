#include <vinc/concurrency/detail/error.hpp>
#include <vinc/concurrency/detail/error_catalog.hpp>
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <vinc/osal/error.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using vinc::concurrency::ErrorCode;
using vinc::concurrency::ErrorOperation;
using vinc::concurrency::detail::translate_shared_memory_error;

static_assert(std::is_same_v<decltype(vinc::concurrency::InterprocessMutex::create({}, vinc::central_owner)),
                             vinc::Result<vinc::concurrency::InterprocessMutex>>);
static_assert(static_cast<std::int32_t>(ErrorOperation::Open) == 1);
static_assert(static_cast<std::int32_t>(ErrorOperation::Lock) == 2);
static_assert(static_cast<std::int32_t>(ErrorOperation::Unlock) == 3);
static_assert(static_cast<std::int32_t>(ErrorOperation::Create) == 4);

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

void test_shared_memory_translation() {
    struct Mapping {
        vinc::osal::ErrorCode lower;
        ErrorCode upper;
        std::int32_t stable_code;
    };
    constexpr auto mappings = std::array{
        Mapping{vinc::osal::ErrorCode::InvalidArgument, ErrorCode::InvalidArgument, 1},
        Mapping{vinc::osal::ErrorCode::NotFound, ErrorCode::NotFound, 2},
        Mapping{vinc::osal::ErrorCode::PermissionDenied, ErrorCode::PermissionDenied, 3},
        Mapping{vinc::osal::ErrorCode::ResourceExhausted, ErrorCode::ResourceExhausted, 4},
        Mapping{vinc::osal::ErrorCode::Unsupported, ErrorCode::Unsupported, 5},
        Mapping{vinc::osal::ErrorCode::Timeout, ErrorCode::Timeout, 6},
        Mapping{vinc::osal::ErrorCode::Cancelled, ErrorCode::Cancelled, 7},
        Mapping{vinc::osal::ErrorCode::Busy, ErrorCode::Busy, 9},
        Mapping{vinc::osal::ErrorCode::SystemError, ErrorCode::SystemError, 11},
        Mapping{vinc::osal::ErrorCode::Closed, ErrorCode::Closed, 12},
        Mapping{vinc::osal::ErrorCode::WouldBlock, ErrorCode::WouldBlock, 14},
        Mapping{vinc::osal::ErrorCode::AlreadyExists, ErrorCode::AlreadyExists, 22}};
    // Synthetic native provenance is copied verbatim, regardless of the host OS.
    constexpr vinc::NativeError native{vinc::NativeErrorSource::LinuxErrno, 13};
    for (const auto& mapping : mappings) {
        const auto lower = vinc::make_error(mapping.lower, vinc::osal::ErrorOperation::MapSharedMemory, native);
        const auto upper = translate_shared_memory_error(lower, ErrorOperation::Lock);
        check(upper.is(mapping.upper) && upper.code == mapping.stable_code, "translation/code identity");
        check(!upper.is(mapping.lower), "lower domain leaked into primary classification");
        check(upper.is_operation(ErrorOperation::Lock), "outer operation lost");
        check(!upper.is_operation(vinc::osal::ErrorOperation::MapSharedMemory), "operation domain ignored");
        check(upper.native == native, "native provenance lost");
        check(upper.causes().size() == 1 && upper.causes().front() == lower.record(), "cause lost");
        check(!upper.truncated(), "single cause unexpectedly truncated");
    }
}

void test_unknown_and_nested_causes() {
    constexpr std::int32_t unknown_code = 70001;
    constexpr std::int32_t unknown_operation = 70002;
    constexpr auto unknown_domain = static_cast<vinc::ErrorDomain>(70003);
    constexpr vinc::NativeError native{static_cast<vinc::NativeErrorSource>(70004), -70005};
    const auto unknown = vinc::Error::from_raw(unknown_domain, unknown_code, unknown_operation, native);
    const auto lower = vinc::wrap_error(vinc::osal::ErrorCode::SystemError,
                                         vinc::osal::ErrorOperation::CloseSharedMemory, unknown);
    const auto upper = translate_shared_memory_error(lower, ErrorOperation::Unlock);
    check(upper.is(ErrorCode::SystemError), "nested primary classification");
    check(upper.causes().back() == unknown.record(), "deepest cause lost");
    check(upper.native == native, "foreign native source changed");
    if constexpr (vinc::config::error_chain_max_depth > 2) {
        check(upper.causes().front() == lower.record() && !upper.truncated(), "OSAL cause lost");
    } else {
        check(upper.truncated(), "omitted OSAL cause not marked");
    }

    constexpr auto foreign_cases = std::array{
        // Same numeric code as NotFound, but not the OSAL domain.
        vinc::Error::from_raw(unknown_domain, 2, unknown_operation, native),
        vinc::Error::from_raw(vinc::ErrorDomain::Osal, unknown_code, unknown_operation, native)};
    for (const auto& failure : foreign_cases) {
        const auto translated = translate_shared_memory_error(failure, ErrorOperation::Open);
        check(translated.is(ErrorCode::SystemError), "unknown failure needs local fallback");
        check(translated.causes().front() == failure.record(), "unknown numeric record lost");
    }

    constexpr auto unrelated_resource_codes = std::array{
        vinc::osal::ErrorCode::ServiceClosed, vinc::osal::ErrorCode::ObservationLost,
        vinc::osal::ErrorCode::PeerClosed, vinc::osal::ErrorCode::AddressInUse,
        vinc::osal::ErrorCode::AddressNotAvailable, vinc::osal::ErrorCode::ConnectionRefused,
        vinc::osal::ErrorCode::ConnectionReset, vinc::osal::ErrorCode::NotConnected,
        vinc::osal::ErrorCode::MessageTooLarge, vinc::osal::ErrorCode::NetworkUnreachable};
    for (const auto code : unrelated_resource_codes) {
        const auto failure = vinc::make_error(code);
        const auto translated = translate_shared_memory_error(failure, ErrorOperation::Create);
        check(translated.is(ErrorCode::SystemError), "unrelated OSAL category exposed by mutex");
        check(translated.causes().front() == failure.record(), "unexpected OSAL cause lost");
    }

    const auto text = vinc::error_to_string(upper);
    check(text.contains("Concurrency") && text.contains("Unlock"), "outer catalog missing");
    check(text.contains("70003") && text.contains("70001") && text.contains("-70005"),
          "unknown diagnostics lost");
}

void test_selected_catalog() {
    const auto& catalog = vinc::concurrency::detail::error_catalog;
    for (const auto& code : catalog.codes) {
        const auto failure = vinc::Error::from_raw(catalog.domain, code.value, 0);
        const auto text = vinc::error_to_string(failure);
        check(text.contains("Concurrency") && text.contains(code.text), "error metadata not linked");
    }
    for (const auto& operation : catalog.operations) {
        const auto failure = vinc::Error::from_raw(catalog.domain, static_cast<std::int32_t>(ErrorCode::Closed),
                                                  operation.value);
        check(vinc::error_to_string(failure).contains(operation.text), "operation metadata not linked");
    }
    const auto unknown = vinc::Error::from_raw(catalog.domain, 71001, 71002);
    const auto text = vinc::error_to_string(unknown);
    check(text.contains("71001") && text.contains("71002"), "unknown component values hidden");
}
}

int main() {
    try {
        test_shared_memory_translation();
        test_unknown_and_nested_causes();
        test_selected_catalog();
        std::cout << "PASS: Concurrency error translation, causes and selected catalog\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}

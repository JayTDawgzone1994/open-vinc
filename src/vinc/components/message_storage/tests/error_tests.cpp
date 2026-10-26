// Exercise the public storage error contract and the composed static catalog.
#include <vinc/message/storage.hpp>
#include <vinc/message/detail/framework_access.hpp>
#include <vinc/message/detail/error_catalog.hpp>
#include <vinc/message/detail/image_format.hpp>
#include <vinc/osal/error.hpp>
#include <array>
#include <iostream>
#include <type_traits>

namespace {
using vinc::message::ErrorCode;
using vinc::message::ErrorOperation;

bool check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
    }

    return condition;
}

template <class T> bool check_error(const vinc::Result<T>& result, ErrorCode code, ErrorOperation operation) {
    if (!check(!result, "expected a failed Result")) {
        return false;
    }

    return check(result.error().is(code) && result.error().is_operation(operation),
                 "storage failure must preserve its domain, code and operation");
}

// Corrupt encoded bytes without relying on native alignment or object layout.
void store_integer(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0; i < vinc::message::detail::format::integer_bytes; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 255);
    }
}

bool growth_failure_test(bool scratch_is_limit) {
    using Framework = vinc::message::detail::FrameworkAccess;
    namespace format = vinc::message::detail::format;
    constexpr std::size_t element_count = 3;
    constexpr std::uint64_t sample_value = 71;
    // One root, one descriptor and three uint64 elements fit exactly. Geometric
    // growth reserves four elements, so the first attempt must fail and retry.
    constexpr std::size_t exact_bytes = format::header::size + format::integer_bytes +
                                        format::descriptor::size + element_count * sizeof(std::uint64_t);
    const auto backing_bytes = scratch_is_limit ? exact_bytes * 2 : exact_bytes;
    auto backing = std::make_shared<std::vector<std::byte>>(backing_bytes);
    auto message = Framework::borrow(*backing, backing, exact_bytes);
    if (!check(bool(message), "create bounded loan")) {
        return false;
    }

    auto array = message->add_array<std::uint64_t>();
    if (!check(bool(array), "create bounded array")) {
        return false;
    }

    if (!check(bool(array->resize(element_count)), "exact growth retry must succeed") ||
        !check(array->capacity() == element_count, "retry uses exact capacity") ||
        !check(bool(array->set(0, sample_value)), "write sample value")) {
        return false;
    }

    auto saved_proxy = (*array)[0];
    const auto before = *backing;
    const auto code = scratch_is_limit ? ErrorCode::ResourceExhausted : ErrorCode::CapacityExceeded;
    bool passed = check_error(array->push_back(sample_value), code, ErrorOperation::Resize);
    passed &= check(*backing == before, "failed append preserves the complete borrowed image");
    passed &= check(static_cast<std::uint64_t>(saved_proxy) == sample_value,
                    "failed append preserves prior element handles");
    passed &= check(array->size() == element_count && array->capacity() == element_count,
                    "failed append preserves descriptor state");
    passed &= check_error(array->copy_from(std::vector<std::uint64_t>(element_count + 1)), code,
                          ErrorOperation::Import);
    passed &= check(*backing == before, "failed import preserves the borrowed image");

    Framework::revoke(*message); // Contract probes cover subsequent invalid access.
    return passed;
}

bool malformed_result_tests() {
    using Framework = vinc::message::detail::FrameworkAccess;
    namespace detail = vinc::message::detail;
    namespace format = detail::format;
    constexpr std::size_t loan_bytes = 256;
    auto backing = std::make_shared<std::vector<std::byte>>(loan_bytes);
    auto message = Framework::borrow(*backing, backing, loan_bytes);
    if (!check(bool(message), "create empty loan")) {
        return false;
    }

    // seal must reject a corrupt length before span::first, without revoking the
    // writer. Repairing only the header must allow the original loan to seal.
    store_integer(*backing, format::header::total_bytes, UINT64_MAX);
    bool passed = check_error(Framework::seal(*message), ErrorCode::InvalidData, ErrorOperation::Seal);
    passed &= check(bool(message->usage()), "failed seal retains the active loan");
    store_integer(*backing, format::header::total_bytes, format::header::size);
    auto sealed = Framework::seal(*message);
    if (!check(bool(sealed), "repaired loan can seal")) {
        return false;
    }

    std::vector<std::byte> malformed(sealed->bytes().begin(), sealed->bytes().end());
    store_integer(malformed, format::header::node_count, UINT64_MAX);
    passed &= check_error(vinc::message::Snapshot::copy_from(malformed), ErrorCode::InvalidData,
                          ErrorOperation::ValidateImage);
    detail::Schema empty_schema{1, {}};
    passed &= check_error(detail::adopt_record(malformed, empty_schema), ErrorCode::InvalidData,
                          ErrorOperation::ValidateImage);

    detail::Schema cyclic_schema{1, {}};
    cyclic_schema.fields.push_back({"recursive", {0, 0, &cyclic_schema, cyclic_schema.hash}, false});
    passed &=
        check_error(detail::create_record(cyclic_schema), ErrorCode::SizeOverflow, ErrorOperation::AddRoot);
    detail::Schema invalid_schema{1, {{"invalid", {99, 1, nullptr, 0}, true}}};
    passed &=
        check_error(detail::create_record(invalid_schema), ErrorCode::TypeMismatch, ErrorOperation::AddRoot);
    return passed;
}

bool nested_export_test() {
    vinc::MessageArray<vinc::MessageArray<std::uint32_t>> source;
    if (!check(bool(source.resize(1)), "create nested source") ||
        !check(bool(source[0].push_back(42)), "populate nested source")) {
        return false;
    }

    auto exported = source.to_vector();
    if (!check(bool(exported), "nested export returns Result")) {
        return false;
    }

    bool passed = check(bool((*exported)[0].set(0, 99)), "edit exported copy");
    auto original = source[0].at(0);
    passed &= check(original && *original == 42, "nested export owns independent local storage");
    return passed;
}

}

static_assert(std::same_as<std::underlying_type_t<ErrorCode>, std::int32_t>);
static_assert(
    std::same_as<decltype(std::declval<vinc::MessageArray<std::uint8_t>&>().resize(1)), vinc::Result<void>>);
static_assert(static_cast<std::int32_t>(ErrorCode::CapacityExceeded) == 0);
static_assert(static_cast<std::int32_t>(ErrorCode::InvalidArgument) == 8);
static_assert(vinc::valid_catalog(vinc::message::detail::error_catalog));

int main() {
    bool passed = true;
    const vinc::Error capacity{ErrorCode::CapacityExceeded, ErrorOperation::Resize};
    const vinc::Result<void> failed = std::unexpected(capacity);
    passed &= check_error(failed, ErrorCode::CapacityExceeded, ErrorOperation::Resize);
    passed &= check(vinc::Result<void>{}.has_value(), "success belongs to Result's value state");
    const auto text = vinc::error_to_string(capacity);
    passed &= check(text.find("Message capacity exceeded") != std::string::npos &&
                        text.find("Resize") != std::string::npos && text.find("code=0") != std::string::npos,
                    "zero code and operation must resolve through the composed catalog");

    const auto foreign = vinc::Error::from_raw(vinc::ErrorDomain::Osal, capacity.code, capacity.operation);
    const auto decode_text = vinc::error_to_string(
        vinc::Error{ErrorCode::InvalidData, ErrorOperation::Deserialize});
    passed &= check(decode_text.find("Deserialize") != std::string::npos,
                    "deserialize operation missing from static catalog");
    passed &= check(!foreign.is(ErrorCode::CapacityExceeded), "matching must include the domain");
    const auto unknown = vinc::Error::from_raw(vinc::ErrorDomain::MessageStorage, -71, -19);
    const auto unknown_text = vinc::error_to_string(unknown);
    passed &= check(unknown_text.find("code=-71") != std::string::npos &&
                        unknown_text.find("operation=-19") != std::string::npos,
                    "unknown identifiers must remain numeric");

    const auto wrapped = vinc::wrap_error(vinc::osal::ErrorCode::InvalidArgument,
                                          vinc::osal::ErrorOperation::Unspecified, capacity);
    passed &= check(wrapped.causes().size() == 1 && wrapped.causes().front() == capacity.record(),
                    "upper layers can preserve the storage error as a cause");
    passed &= check(vinc::error_to_string(wrapped).find("Message capacity exceeded") != std::string::npos,
                    "storage cause text must resolve through the same catalog");

    // Validate catalog rules independently of the runtime: zero code is permitted,
    // but duplicate codes and the unspecified operation are not valid table entries.
    constexpr std::array zero_codes{vinc::ErrorCodeEntry{ErrorCode::CapacityExceeded, "Capacity"}};
    constexpr std::array zero_operations{vinc::ErrorOperationEntry{ErrorOperation::Unspecified, "Unspecified"}};
    constexpr std::array duplicate_codes{zero_codes[0], zero_codes[0]};
    static_assert(vinc::valid_catalog({vinc::ErrorDomain::MessageStorage, "Storage", zero_codes, {}}));
    static_assert(!vinc::valid_catalog({vinc::ErrorDomain::MessageStorage, "Storage", duplicate_codes, {}}));
    static_assert(
        !vinc::valid_catalog({vinc::ErrorDomain::MessageStorage, "Storage", zero_codes, zero_operations}));

    vinc::MessageArray<std::uint32_t> array;
    passed &= check_error(array.at(0), ErrorCode::OutOfRange, ErrorOperation::Read);
    passed &= check_error(array.pop_back(), ErrorCode::OutOfRange, ErrorOperation::PopBack);
    passed &= check_error(array.resize(UINT64_MAX), ErrorCode::SizeOverflow, ErrorOperation::Resize);
    passed &= check_error(array.reserve(UINT64_MAX), ErrorCode::SizeOverflow, ErrorOperation::Reserve);
    passed &= check_error(array.snapshot(1), ErrorCode::CapacityExceeded, ErrorOperation::Snapshot);
    passed &= check_error(vinc::message::Snapshot::copy_from({}), ErrorCode::InvalidData,
                          ErrorOperation::ValidateImage);
    passed &= check_error(vinc::message::detail::FrameworkAccess::borrow({}, {}, 0),
                          ErrorCode::InvalidArgument, ErrorOperation::Borrow);

    vinc::message::Message local;
    passed &= check_error(vinc::message::detail::FrameworkAccess::seal(local), ErrorCode::InvalidArgument,
                          ErrorOperation::Seal);
    passed &= growth_failure_test(false);
    passed &= growth_failure_test(true);
    passed &= malformed_result_tests();
    passed &= nested_export_test();
    return passed ? 0 : 1;
}

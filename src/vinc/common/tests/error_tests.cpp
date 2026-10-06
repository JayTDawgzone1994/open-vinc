#include <vinc/result.hpp>
#include <vinc/error_catalog.hpp>
#include <array>
#include <iostream>
#include <type_traits>

namespace fixture {
enum class Code : std::int32_t {
    Failure = 1
};
enum class Operation : std::int32_t {
    Execute = 1
};
enum class OtherCode : std::int32_t {
    Failure = 1
};
enum class OtherOperation : std::int32_t {
    Execute = 1
};
}

namespace vinc {
template <> struct ErrorCodeTraits<fixture::Code> {
    static constexpr ErrorDomain domain = ErrorDomain::Osal;
};

template <> struct ErrorOperationTraits<fixture::Operation> {
    static constexpr ErrorDomain domain = ErrorDomain::Osal;
};

template <> struct ErrorCodeTraits<fixture::OtherCode> {
    static constexpr ErrorDomain domain = ErrorDomain::ControlChannel;
};

template <> struct ErrorOperationTraits<fixture::OtherOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::ControlChannel;
};
}

namespace {
constexpr auto codes = std::array{vinc::ErrorCodeEntry{fixture::Code::Failure, "Fixture failure"}};
constexpr auto operations = std::array{vinc::ErrorOperationEntry{fixture::Operation::Execute, "Execute"}};
constexpr vinc::ErrorCatalog catalog{vinc::ErrorDomain::Osal, "Fixture", codes, operations};
constexpr auto catalogs = std::array{catalog};
constexpr auto duplicates = std::array{codes[0], codes[0]};
constexpr auto duplicate_domains = std::array{catalog, catalog};
constexpr auto foreign = std::array{vinc::ErrorCodeEntry{fixture::OtherCode::Failure, "Foreign"}};
constexpr auto empty_text = std::array{vinc::ErrorCodeEntry{fixture::Code::Failure, ""}};
static_assert(vinc::valid_catalogs(catalogs));
static_assert(!vinc::valid_catalogs(duplicate_domains));
static_assert(!vinc::valid_entries(vinc::ErrorDomain::Osal, duplicates));
static_assert(!vinc::valid_entries(vinc::ErrorDomain::Osal, foreign));
static_assert(!vinc::valid_entries(vinc::ErrorDomain::Osal, empty_text));
static_assert(!std::is_constructible_v<vinc::Error, fixture::Code, fixture::OtherOperation>);
static_assert(!std::is_constructible_v<vinc::ErrorCodeEntry, fixture::Operation, std::string_view>);
static_assert(!std::is_constructible_v<vinc::ErrorOperationEntry, fixture::Code, std::string_view>);
static_assert(!std::is_constructible_v<vinc::ErrorCatalog, vinc::ErrorDomain, std::string_view,
                                      std::span<const vinc::ErrorOperationEntry>,
                                      std::span<const vinc::ErrorCodeEntry>>);
static_assert(std::is_trivially_copyable_v<vinc::Error>);
static_assert(std::is_nothrow_copy_constructible_v<vinc::Error>);
static_assert(std::is_same_v<vinc::Result<int>, std::expected<int, vinc::Error>>);

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }

    return condition;
}

vinc::Result<void> forward(const vinc::Result<int>& input) {
    if (!input) {
        return std::unexpected(input.error());
    }

    return {};
}
}

int main() {
    bool passed = true;
    const auto failure = vinc::make_error(fixture::Code::Failure, fixture::Operation::Execute,
                                          vinc::NativeError{static_cast<vinc::NativeErrorSource>(999), -123});
    const vinc::Result<int> failed = std::unexpected(failure);
    const auto copied = forward(failed);
    passed &= check(!copied && copied.error() == failure, "propagation must preserve all identity fields");
    passed &= check(forward(vinc::Result<int>{42}).has_value(), "value state must remain success");
    passed &= check(failure.is(fixture::Code::Failure) && !failure.is(fixture::OtherCode::Failure),
                    "equal local numbers from different domains must not match");
    passed &= check(failure.is_operation(fixture::Operation::Execute), "typed operation match");
    const auto text = vinc::detail::format_error(failure, catalogs);
    passed &= check(text.find("Fixture failure") != std::string::npos &&
                        text.find("Execute") != std::string::npos &&
                        text.find("native=999:-123") != std::string::npos,
                    "static text and unknown native provenance must survive");
    const auto unknown = vinc::Error::from_raw(static_cast<vinc::ErrorDomain>(1234), -12345, -6789);
    const auto unknown_text = vinc::error_to_string(unknown);
    passed &= check(unknown_text.find("domain=1234") != std::string::npos &&
                        unknown_text.find("code=-12345") != std::string::npos &&
                        unknown_text.find("operation=-6789") != std::string::npos,
                    "missing catalog must preserve unknown signed identifiers");
    const vinc::Result<void> empty_error = std::unexpected(vinc::Error{});
    passed &= check(!empty_error, "default Error must never mean success");
    const auto empty_text = vinc::error_to_string(empty_error.error());
    passed &= check(empty_text.find("code=0") != std::string::npos, "unknown zero code remains numeric");
    return passed ? 0 : 1;
}

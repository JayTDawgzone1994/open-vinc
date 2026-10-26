#include <vinc/error_catalog.hpp>
#include <vinc/result.hpp>
#include <algorithm>
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
enum class OtherOperation : std::int32_t {
    Execute = 1
};
}

namespace vinc {
template <> struct ErrorCodeTraits<fixture::Code> {
    static constexpr ErrorDomain domain = ErrorDomain::ModuleSystem;
};

template <> struct ErrorOperationTraits<fixture::Operation> {
    static constexpr ErrorDomain domain = ErrorDomain::ModuleSystem;
};

template <> struct ErrorOperationTraits<fixture::OtherOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::Osal;
};
}

namespace {
using namespace vinc;
constexpr std::size_t overflow_layers = 4;
constexpr NativeError native{static_cast<NativeErrorSource>(999), -123456789};
constexpr auto root = Error::from_raw(static_cast<ErrorDomain>(12345), -17, -29, native);
constexpr auto wrapped = wrap_error(fixture::Code::Failure, fixture::Operation::Execute, root);
static_assert(wrapped.causes().size() == 1);
static_assert(wrapped.causes().front() == root.record());
static_assert(wrapped.native == root.native);
static_assert(!wrapped.truncated());
static_assert(std::is_trivially_copyable_v<Error>);
static_assert(std::is_standard_layout_v<Error>);
static_assert(std::is_nothrow_copy_constructible_v<Error>);
static_assert(noexcept(wrap_error(fixture::Code::Failure, fixture::Operation::Execute, root)));
template <class Code, class Operation>
concept CanWrap =
    requires(Code code, Operation operation, const Error& cause) { wrap_error(code, operation, cause); };
static_assert(!CanWrap<fixture::Code, int>);
static_assert(!CanWrap<fixture::Code, fixture::OtherOperation>);

bool check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << description << '\n';
    }

    return condition;
}

Result<void> forward(const Result<int>& input) {
    if (!input) {
        return std::unexpected(input.error());
    }

    return {};
}

bool test_chain_order() {
    bool passed = true;
    auto chain = root;
    for (std::size_t layer = 1; layer <= config::error_chain_max_depth + overflow_layers; ++layer) {
        const auto previous = chain;
        chain = wrap_error(static_cast<fixture::Code>(layer), fixture::Operation::Execute, previous);
        const auto causes = chain.causes();
        const auto retained = (std::min)(layer, config::error_chain_max_depth - 1);
        passed &= check(causes.size() == retained, "chain exceeded its configured bound");
        passed &= check(causes.back() == root.record(), "deepest cause was lost");
        passed &= check(chain.truncated() == (layer >= config::error_chain_max_depth),
                        "truncation state does not match omitted layers");
        for (std::size_t index = 0; index + 1 < causes.size(); ++index) {
            passed &= check(causes[index].domain == ErrorDomain::ModuleSystem &&
                                causes[index].code == static_cast<std::int32_t>(layer - index - 1),
                            "nearest retained causes are out of order");
        }
        const Result<int> failed = std::unexpected(chain);
        const auto forwarded = forward(failed);
        passed &= check(!forwarded && forwarded.error() == chain,
                        "plain Result propagation changed or grew the chain");
        passed &= check(chain.native == native, "wrapping lost outer native provenance");
    }
    return passed;
}

bool test_import_and_formatting() {
    bool passed = true;
    std::array<ErrorRecord, config::error_chain_max_depth + overflow_layers> records{};
    for (std::size_t index = 0; index < records.size(); ++index) {
        records[index] = {
            static_cast<ErrorDomain>(20000 + index), -static_cast<std::int32_t>(index + 1), 0, {}};
    }
    const auto imported = Error::from_raw(ErrorDomain::ModuleSystem, 1, 1, {}, records);
    passed &= check(imported.truncated() && imported.causes().back() == records.back(),
                    "larger remote capacity must retain deepest cause and mark omission");
    // Even a receiver with room must preserve an omission made by a smaller sender.
    const std::array short_chain{root.record()};
    const auto already_truncated =
        Error::from_raw(ErrorDomain::ModuleSystem, 1, 1, native, short_chain, true);
    const auto rewrapped = wrap_error(fixture::Code::Failure, fixture::Operation::Execute, already_truncated);
    passed &= check(rewrapped.truncated() && rewrapped.causes().back() == root.record(),
                    "rewrapping erased an earlier sender's omission");

    constexpr std::array codes{ErrorCodeEntry{fixture::Code::Failure, "Outer failure"}};
    constexpr std::array operations{ErrorOperationEntry{fixture::Operation::Execute, "Execute"}};
    const std::array catalogs{ErrorCatalog{ErrorDomain::ModuleSystem, "ModuleSystem", codes, operations}};
    const auto text = detail::format_error(rewrapped, catalogs);
    const auto omission = text.find("[intermediate causes omitted]");
    const auto root_position = text.find("domain=12345");
    passed &= check(text.starts_with("ModuleSystem") && omission != std::string::npos &&
                        root_position != std::string::npos && omission < root_position &&
                        text.find("code=-17") != std::string::npos &&
                        text.find("operation=-29") != std::string::npos &&
                        text.find("native=999:-123456789") != std::string::npos,
                    "chain formatting lost order, unknown identity, or omission marker");
    passed &= check(wrapped.is(fixture::Code::Failure) && wrapped.domain != wrapped.causes().back().domain,
                    "typed matching must inspect only the current record");
    const auto different_outer =
        wrap_error(static_cast<fixture::Code>(2), static_cast<fixture::Operation>(2), wrapped);
    passed &= check(!different_outer.is(fixture::Code::Failure) &&
                        !different_outer.is_operation(fixture::Operation::Execute),
                    "matching must not find a code or operation in the cause chain");
    const auto empty = Error{};
    passed &= check(empty.causes().empty() && !empty.truncated(), "default error has unexpected causes");
    passed &= check(error_to_string(empty).find("<-") == std::string::npos,
                    "single-record formatting must not add a chain marker");
    const auto copied = imported;
    passed &= check(copied == imported && copied.causes().data() != imported.causes().data(),
                    "copied error must own its own cause storage");
    return passed;
}
}

int main() {
    bool passed = test_chain_order();
    passed &= test_import_and_formatting();
    std::cout << "depth=" << vinc::config::error_chain_max_depth << " Error=" << sizeof(vinc::Error)
              << " Result<void>=" << sizeof(vinc::Result<void>)
              << " Result<int>=" << sizeof(vinc::Result<int>) << '\n';
    return passed ? 0 : 1;
}

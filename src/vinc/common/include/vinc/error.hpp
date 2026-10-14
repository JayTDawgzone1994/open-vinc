#pragma once

#include <vinc/config.hpp>
#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

namespace vinc {
// Stable identities, independent of build order and enabled components.
enum class ErrorDomain : std::uint32_t {
    Unknown = 0,
    Osal = 1,
    ControlChannel = 2,
    ModuleSystem = 3,
    MessageBus = 4,
    MessageStorage = 5,
    Concurrency = 6,
    OutputHub = 7,
    Shell = 8,
    Common = 9,
    Param = 10,
    Log = 11,
    Bag = 12,
    Ui = 13,
    Rpc = 14
};

// Each source identifies a numeric namespace, including its originating platform.
// POSIX namespaces describe the currently supported x86-64 native ABI.
enum class NativeErrorSource : std::uint32_t {
    None = 0,
    Win32 = 1,
    Winsock = 2,
    NtStatus = 3,
    LinuxErrno = 4,
    DarwinErrno = 5,
    LinuxResolver = 6,
    DarwinResolver = 7,
    LinuxGeneric = 8,
    DarwinGeneric = 9,
    WindowsGeneric = 10,
    Sqlite = 11
};

struct NativeError {
    NativeErrorSource source = NativeErrorSource::None;
    // Preserve negative resolver values and unsigned Win32 values without truncation.
    std::int64_t code = 0;

    constexpr explicit operator bool() const noexcept {
        return source != NativeErrorSource::None || code != 0;
    }

    constexpr std::int64_t value() const noexcept {
        return code;
    }

    friend bool operator==(const NativeError&, const NativeError&) = default;
};

template <class T> struct ErrorCodeTraits;
template <class T> struct ErrorOperationTraits;

template <class T>
concept ErrorCodeEnum = std::is_enum_v<T> && requires {
    { ErrorCodeTraits<T>::domain } -> std::convertible_to<ErrorDomain>;
} && std::same_as<std::underlying_type_t<T>, std::int32_t>;

template <class T>
concept ErrorOperationEnum = std::is_enum_v<T> && requires {
    { ErrorOperationTraits<T>::domain } -> std::convertible_to<ErrorDomain>;
} && std::same_as<std::underlying_type_t<T>, std::int32_t>;

// One numeric diagnostic record. It owns no strings or process-local pointers.
struct ErrorRecord {
    ErrorDomain domain = ErrorDomain::Unknown;
    std::int32_t code = 0;
    std::int32_t operation = 0;
    NativeError native{};

    friend bool operator==(const ErrorRecord&, const ErrorRecord&) = default;
};

struct Error;

namespace detail {
// Private counters keep borrowed views bounded. All data members have the same
// access level so this storage, and enclosing native I/O records, retain standard layout.
class ErrorCauseStorage {
public:
    constexpr ErrorCauseStorage() noexcept = default;
    friend bool operator==(const ErrorCauseStorage&, const ErrorCauseStorage&) = default;

private:
    friend struct vinc::Error;
    std::array<ErrorRecord, config::error_chain_max_depth - 1> records_{};
    std::size_t count_ = 0;
    bool truncated_ = false;
};
}

// Bounded value-only cause chain. Copying and wrapping require no allocation.
// Explicitly encode fields for IPC: the C++ object layout is not a wire ABI.
struct Error {
    ErrorDomain domain = ErrorDomain::Unknown;
    std::int32_t code = 0;
    std::int32_t operation = 0;
    NativeError native{};
    // Public solely to preserve standard layout for existing native I/O containers.
    // Use causes()/truncated(); the storage's records and counters are private.
    detail::ErrorCauseStorage cause_storage{};

    constexpr Error() noexcept = default;

    template <ErrorCodeEnum Code>
    constexpr explicit Error(Code value) noexcept
        : domain(ErrorCodeTraits<Code>::domain), code(static_cast<std::int32_t>(value)) {
    }

    template <ErrorCodeEnum Code, ErrorOperationEnum Operation>
        requires(ErrorCodeTraits<Code>::domain == ErrorOperationTraits<Operation>::domain)
    constexpr Error(Code value, Operation action, NativeError cause = {}) noexcept
        : domain(ErrorCodeTraits<Code>::domain), code(static_cast<std::int32_t>(value)),
          operation(static_cast<std::int32_t>(action)), native(cause) {
    }

    // Transport adapters validate their envelope first, then retain unknown identifiers.
    // Causes are nearest-first; any incoming omission belongs immediately before the
    // final cause. Oversized input keeps the nearest causes and the deepest record.
    // Precondition: a truncated chain contains at least its deepest cause.
    static constexpr Error from_raw(ErrorDomain domain, std::int32_t code, std::int32_t operation,
                                    NativeError native = {}, std::span<const ErrorRecord> causes = {},
                                    bool truncated = false) noexcept {
        assert(!truncated || !causes.empty());
        Error result;
        result.domain = domain;
        result.code = code;
        result.operation = operation;
        result.native = native;
        result.assign_causes(causes, truncated);
        return result;
    }

    template <ErrorCodeEnum Code> constexpr bool is(Code value) const noexcept {
        return domain == ErrorCodeTraits<Code>::domain && code == static_cast<std::int32_t>(value);
    }

    template <ErrorOperationEnum Operation> constexpr bool is_operation(Operation value) const noexcept {
        return domain == ErrorOperationTraits<Operation>::domain &&
               operation == static_cast<std::int32_t>(value);
    }

    constexpr ErrorRecord record() const noexcept {
        return {domain, code, operation, native};
    }

    // The view borrows this Error and expires when its owner is destroyed or changed.
    // Rvalue access is disallowed to avoid immediately dangling views.
    constexpr std::span<const ErrorRecord> causes() const& noexcept {
        return std::span<const ErrorRecord>(cause_storage.records_).first(cause_storage.count_);
    }

    std::span<const ErrorRecord> causes() const&& = delete;

    constexpr bool truncated() const noexcept {
        return cause_storage.truncated_;
    }

    friend bool operator==(const Error&, const Error&) = default;

private:
    constexpr void assign_causes(std::span<const ErrorRecord> causes, bool truncated) noexcept {
        const bool exceeds_capacity = causes.size() > cause_storage.records_.size();
        cause_storage.count_ = exceeds_capacity ? cause_storage.records_.size() : causes.size();
        cause_storage.truncated_ = truncated || exceeds_capacity;
        for (std::size_t index = 0; index < cause_storage.count_; ++index) {
            cause_storage.records_[index] = causes[index];
        }
        if (exceeds_capacity) {
            // Preserve the deepest known cause; the gap is directly before it.
            cause_storage.records_[cause_storage.count_ - 1] = causes.back();
        }
    }
};

template <ErrorCodeEnum Code> constexpr Error make_error(Code code) noexcept {
    return Error(code);
}

template <ErrorCodeEnum Code, ErrorOperationEnum Operation>
    requires(ErrorCodeTraits<Code>::domain == ErrorOperationTraits<Operation>::domain)
constexpr Error make_error(Code code, Operation operation, NativeError native = {}) noexcept {
    return Error(code, operation, native);
}

// Translate at a component boundary. Matching still inspects only the new outer
// identity; diagnostic causes never become the caller's primary classification.
template <ErrorCodeEnum Code, ErrorOperationEnum Operation>
    requires(ErrorCodeTraits<Code>::domain == ErrorOperationTraits<Operation>::domain)
constexpr Error wrap_error(Code code, Operation operation, const Error& cause) noexcept {
    std::array<ErrorRecord, config::error_chain_max_depth> records{};
    records[0] = cause.record();
    const auto nested = cause.causes();
    for (std::size_t index = 0; index < nested.size(); ++index) {
        records[index + 1] = nested[index];
    }
    return Error::from_raw(ErrorCodeTraits<Code>::domain, static_cast<std::int32_t>(code),
                           static_cast<std::int32_t>(operation), cause.native,
                           std::span<const ErrorRecord>(records).first(nested.size() + 1), cause.truncated());
}

// Defined by the selected static catalog. Formatting may allocate/throw.
std::string error_to_string(const Error& error);
}

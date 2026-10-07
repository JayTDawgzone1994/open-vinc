#include <vinc/module/detail/contract_report.hpp>
#include <vinc/module/error.hpp>
#include <vinc/osal/shared_atomic.hpp>
#include <cstdlib>
#include <memory>
#include <thread>
#ifdef VINC_MODULE_TESTING
#include "fault_hooks.hpp"
#endif

namespace vinc::detail {
namespace {
using module_system::ErrorCode;
using module_system::ErrorOperation;
constexpr std::uint64_t empty = 0;
constexpr std::uint64_t writing = 1;
constexpr std::uint64_t committed = 2;
constexpr std::uint64_t format_version = 1;
constexpr std::size_t word_bytes = 8;
constexpr std::size_t version_offset = 8;
constexpr std::size_t mid_offset = 16;
constexpr std::size_t flags_offset = 24;
constexpr std::size_t line_offset = 32;
constexpr std::size_t index_offset = 40;
constexpr std::size_t size_offset = 48;
constexpr std::size_t text_offset = 56;
constexpr std::size_t report_bytes = text_offset + ModuleContractReport::reason_capacity +
                                     ModuleContractReport::file_capacity + ModuleContractReport::function_capacity;
constexpr std::uint64_t index_flag = 1;
constexpr std::uint64_t truncated_flag = 2;

osal::SharedWord& state(std::span<std::byte> bytes) noexcept {
    // OS mappings are page-aligned. Only this 8-byte native IPC atomic uses
    // object layout; central constructs it before sharing. Payload is LE bytes.
    return *reinterpret_cast<osal::SharedWord*>(bytes.data());
}

void put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t byte = 0; byte < word_bytes; ++byte) {
        bytes[offset + byte] = static_cast<std::byte>((value >> (byte * 8)) & 0xff);
    }
}

std::uint64_t get(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < word_bytes; ++byte) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset + byte])) << (byte * 8);
    }
    return value;
}

bool put_text(std::span<std::byte> bytes, std::size_t offset, std::size_t capacity,
              const char* text) noexcept {
    std::size_t length = 0;
    if (text) {
        while (length + 1 < capacity && text[length] != '\0') {
            bytes[offset + length] = static_cast<std::byte>(text[length]);
            ++length;
        }
    }

    bytes[offset + length] = std::byte{0};
    return text && text[length] != '\0';
}

template <std::size_t Capacity>
bool get_text(std::span<const std::byte> bytes, std::size_t offset,
              std::array<char, Capacity>& text) noexcept {
    for (std::size_t index = 0; index < Capacity; ++index) {
        text[index] = static_cast<char>(std::to_integer<unsigned char>(bytes[offset + index]));
        if (text[index] == '\0') {
            return true;
        }
    }
    return false;
}

Error storage_error(const Error& cause) noexcept {
    return wrap_error(ErrorCode::FaultStorageFailed, ErrorOperation::PrepareFaultReport, cause);
}
}

ContractReportStorage::ContractReportStorage(std::string name, MID mid)
    : name_(std::move(name)), mid_(mid) {
}

ContractReportStorage::~ContractReportStorage() {
    (void)close();
}

Result<void> ContractReportStorage::map() {
    auto mapped = memory_.map(report_bytes, osal::SharedMemoryAccess::ReadWrite);
    if (!mapped) {
        return std::unexpected(storage_error(mapped.error()));
    }

    mapping_ = std::move(*mapped);
    auto bytes = mapping_.writable_bytes();
    if (!bytes) {
        return std::unexpected(storage_error(bytes.error()));
    }

    bytes_ = *bytes;
    return {};
}

Result<std::unique_ptr<ContractReportStorage>> ContractReportStorage::create(std::string name, MID mid) {
    // Allocate the owner before acquiring a named resource; failures then unwind it.
    auto owner = std::unique_ptr<ContractReportStorage>(new ContractReportStorage(std::move(name), mid));
    auto memory = osal::SharedMemory::create(owner->name_, report_bytes);
    if (!memory) {
        return std::unexpected(storage_error(memory.error()));
    }

    owner->memory_ = std::move(*memory);
    owner->owns_name_ = true;
    auto mapped = owner->map();
    if (!mapped) {
        return std::unexpected(mapped.error());
    }

    std::construct_at(&state(owner->bytes_));
    put(owner->bytes_, version_offset, format_version);
    put(owner->bytes_, mid_offset, mid);
    return owner;
}

Result<std::unique_ptr<ContractReportStorage>> ContractReportStorage::open(std::string name, MID mid) {
    auto owner = std::unique_ptr<ContractReportStorage>(new ContractReportStorage(std::move(name), mid));
    auto memory = osal::SharedMemory::open(owner->name_, osal::SharedMemoryAccess::ReadWrite);
    if (!memory) {
        return std::unexpected(storage_error(memory.error()));
    }

    owner->memory_ = std::move(*memory);
    auto mapped = owner->map();
    if (!mapped) {
        return std::unexpected(mapped.error());
    }

    if (get(owner->bytes_, version_offset) != format_version || get(owner->bytes_, mid_offset) != mid ||
        osal::shared_load(state(owner->bytes_)) != empty) {
        return std::unexpected(Error{ErrorCode::InvalidFaultReport, ErrorOperation::PrepareFaultReport});
    }

    return owner;
}

const std::string& ContractReportStorage::name() const noexcept {
    return name_;
}

Result<void> ContractReportStorage::close() noexcept {
    std::optional<Error> failure;
    // Only the successful creator removes a name; child destruction cannot remove it.
    if (owns_name_) {
#ifndef _WIN32
        try {
            auto unlinked = osal::SharedMemory::unlink_name(name_);
            if (!unlinked && !unlinked.error().is(osal::ErrorCode::NotFound)) {
                failure = wrap_error(ErrorCode::CleanupFailed, ErrorOperation::Cleanup, unlinked.error());
            }
        } catch (const std::bad_alloc&) {
            // OSAL name conversion allocates. Cleanup failure cannot terminate
            // central during RAII unwinding; explicit close preserves the fault.
            failure = Error{ErrorCode::ResourceExhausted, ErrorOperation::Cleanup};
        }
#endif
        owns_name_ = false;
    }

    bytes_ = {};
    auto unmapped = mapping_.unmap();
    if (!unmapped && !failure) {
        failure = wrap_error(ErrorCode::CleanupFailed, ErrorOperation::Cleanup, unmapped.error());
    }

    auto closed = memory_.close();
    if (!closed && !failure) {
        failure = wrap_error(ErrorCode::CleanupFailed, ErrorOperation::Cleanup, closed.error());
    }
    if (failure) {
        return std::unexpected(*failure);
    }

    return {};
}

[[noreturn]] void ContractReportStorage::report_and_exit(const ContractViolation& violation) noexcept {
    auto expected = empty;
    if (!osal::shared_compare_exchange(state(bytes_), expected, writing)) {
        // Do not terminate another faulting thread midway through its first report.
        // Wait only on the fatal path. Central supervises the shared writing state
        // independently of this thread and requests termination if its grace expires.
        while (osal::shared_load(state(bytes_)) == writing) {
            std::this_thread::yield();
        }
        std::_Exit(contract_exit_status);
    }

#ifdef VINC_MODULE_TESTING
    module_system::test::checkpoint(module_system::test::Point::ContractWriting);
#endif
    std::size_t offset = text_offset;
    bool truncated = put_text(bytes_, offset, ModuleContractReport::reason_capacity, violation.reason);
    offset += ModuleContractReport::reason_capacity;
    truncated |= put_text(bytes_, offset, ModuleContractReport::file_capacity, violation.file);
    offset += ModuleContractReport::file_capacity;
    truncated |= put_text(bytes_, offset, ModuleContractReport::function_capacity,
                          violation.function);
    put(bytes_, flags_offset, (violation.has_index ? index_flag : 0) | (truncated ? truncated_flag : 0));
    put(bytes_, line_offset, violation.line);
    put(bytes_, index_offset, violation.index);
    put(bytes_, size_offset, violation.size);
    osal::shared_store(state(bytes_), committed);
#ifdef VINC_MODULE_TESTING
    module_system::test::checkpoint(module_system::test::Point::ContractCommitted);
#endif
    // No hooks, stream flushes, heap work or Socket locks after publication.
    std::_Exit(contract_exit_status);
}

Result<ContractReportState> ContractReportStorage::observe_state() const noexcept {
    // Never inspect payload here: a live writer has exclusive access to its bytes.
    if (bytes_.size() != report_bytes) {
        return std::unexpected(Error{ErrorCode::InvalidFaultReport, ErrorOperation::ObserveFaultReport});
    }

    switch (osal::shared_load(state(bytes_))) {
    case empty:
        return ContractReportState::Empty;
    case writing:
        return ContractReportState::Writing;
    case committed:
        return ContractReportState::Committed;
    default:
        return std::unexpected(Error{ErrorCode::InvalidFaultReport, ErrorOperation::ObserveFaultReport});
    }
}

Result<std::optional<ModuleContractReport>> ContractReportStorage::read_after_exit() const noexcept {
    if (bytes_.size() != report_bytes) {
        return std::unexpected(Error{ErrorCode::InvalidFaultReport, ErrorOperation::ReadFaultReport});
    }

    const auto observed = osal::shared_load(state(bytes_));
    if (observed == empty) {
        return std::optional<ModuleContractReport>{};
    }
    if (observed == writing) {
        return std::unexpected(Error{ErrorCode::IncompleteFaultReport, ErrorOperation::ReadFaultReport});
    }

    const auto flags = get(bytes_, flags_offset);
    const auto line = get(bytes_, line_offset);
    if (observed != committed || get(bytes_, version_offset) != format_version ||
        get(bytes_, mid_offset) != mid_ || flags > (index_flag | truncated_flag) || line > UINT32_MAX) {
        return std::unexpected(Error{ErrorCode::InvalidFaultReport, ErrorOperation::ReadFaultReport});
    }

    ModuleContractReport report;
    std::size_t offset = text_offset;
    const bool valid_reason = get_text(bytes_, offset, report.reason);
    offset += report.reason.size();
    const bool valid_file = get_text(bytes_, offset, report.file);
    offset += report.file.size();
    const bool valid_function = get_text(bytes_, offset, report.function);
    if (!valid_reason || !valid_file || !valid_function) {
        return std::unexpected(Error{ErrorCode::InvalidFaultReport, ErrorOperation::ReadFaultReport});
    }

    report.line = static_cast<std::uint32_t>(line);
    report.has_index = (flags & index_flag) != 0;
    report.text_truncated = (flags & truncated_flag) != 0;
    report.index = get(bytes_, index_offset);
    report.size = get(bytes_, size_offset);
    return std::optional{report};
}

}

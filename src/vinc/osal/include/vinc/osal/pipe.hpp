#pragma once

// Owned, asynchronous, one-way byte streams.
// PipeService owns accepted buffers until completion; protocol framing belongs above OSAL.

#include <vinc/osal/error.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace vinc::osal {
struct PipeId {
    std::uint64_t value = 0;
    friend bool operator==(PipeId, PipeId) = default;
};

struct IoId {
    std::uint64_t value = 0;
    friend bool operator==(IoId, IoId) = default;
};
enum class IoKind {
    Read,
    Write
};
enum class IoStatus {
    Completed,
    EndOfStream,
    PeerClosed,
    Cancelled,
    TimedOut,
    Closed,
    Failed
};
enum class CancelResult {
    Requested,
    AlreadyCompleted
};

// Move-only transfer storage. On successful submit, do not retain pointers into it;
// regain access through the buffer returned in IoCompletion.
class IoBuffer {
public:
    IoBuffer() = default;

    explicit IoBuffer(std::size_t size) : storage_(size) {
    }

    IoBuffer(IoBuffer&&) noexcept = default;
    IoBuffer& operator=(IoBuffer&&) noexcept = default;
    IoBuffer(const IoBuffer&) = delete;
    IoBuffer& operator=(const IoBuffer&) = delete;

    std::span<std::byte> bytes() noexcept {
        return storage_;
    }

    std::span<const std::byte> bytes() const noexcept {
        return storage_;
    }

    std::size_t size() const noexcept {
        return storage_.size();
    }

    std::size_t capacity() const noexcept {
        return storage_.capacity();
    }

private:
    std::vector<std::byte> storage_;
};

struct IoCompletion {
    IoId id;
    PipeId pipe;
    IoKind kind = IoKind::Read;
    IoStatus status = IoStatus::Failed;
    // Confirmed transfer count. Failure/cancellation does NOT prove zero
    // delivery; protocol framing must account for partially submitted writes.
    std::size_t transferred = 0;
    std::optional<OsError> error;
    IoBuffer buffer;
};

struct PipeServiceOptions {
    std::size_t max_endpoints = 256;
    std::size_t max_operations = 512; // includes uncollected completions
    std::size_t max_buffered_bytes = 16 * 1024 * 1024;
    std::size_t max_transfer_bytes = 64 * 1024;
};
class PipeService;

namespace detail {
struct PipeState;
struct PipeRecord;
struct PipeWorker;
struct PipeAccess;

// Private owning storage shared by the direction-specific public types.
class PipeEnd {
public:
    PipeEnd() noexcept = default;
    ~PipeEnd();
    PipeEnd(PipeEnd&&) noexcept;
    PipeEnd& operator=(PipeEnd&&) noexcept;
    PipeEnd(const PipeEnd&) = delete;
    PipeEnd& operator=(const PipeEnd&) = delete;
    explicit operator bool() const noexcept;
    PipeId id() const noexcept;
    void close() noexcept; // requests close; native cleanup can complete later
private:
    friend class vinc::osal::PipeService;
    friend struct PipeAccess;
    PipeEnd(std::shared_ptr<PipeState>, std::shared_ptr<PipeRecord>);
    std::shared_ptr<detail::PipeState> state_;
    std::shared_ptr<detail::PipeRecord> record_;
};
} // namespace detail

class PipeReader {
public:
    PipeReader() noexcept = default;
    PipeReader(PipeReader&&) noexcept = default;
    PipeReader& operator=(PipeReader&&) noexcept = default;
    PipeReader(const PipeReader&) = delete;
    PipeReader& operator=(const PipeReader&) = delete;

    explicit operator bool() const noexcept {
        return static_cast<bool>(end_);
    }

    PipeId id() const noexcept {
        return end_.id();
    }

    void close() noexcept {
        end_.close();
    }

private:
    friend class PipeService;
    friend struct detail::PipeAccess;
    detail::PipeEnd end_;
};

class PipeWriter {
public:
    PipeWriter() noexcept = default;
    PipeWriter(PipeWriter&&) noexcept = default;
    PipeWriter& operator=(PipeWriter&&) noexcept = default;
    PipeWriter(const PipeWriter&) = delete;
    PipeWriter& operator=(const PipeWriter&) = delete;

    explicit operator bool() const noexcept {
        return static_cast<bool>(end_);
    }

    PipeId id() const noexcept {
        return end_.id();
    }

    void close() noexcept {
        end_.close();
    }

private:
    friend class PipeService;
    friend struct detail::PipeAccess;
    detail::PipeEnd end_;
};

struct Pipe {
    PipeReader reader;
    PipeWriter writer;
};

class PipeService {
public:
    [[nodiscard]] static Result<PipeService> create(PipeServiceOptions = {});
    ~PipeService(); // closes endpoints; state outlives pending native I/O
    PipeService(PipeService&&) noexcept;
    PipeService& operator=(PipeService&&) noexcept;
    PipeService(const PipeService&) = delete;
    PipeService& operator=(const PipeService&) = delete;
    // One unidirectional byte stream. Two endpoint credits; no child process.
    [[nodiscard]] Result<Pipe> create_pipe();
    // On rejection, buffer remains owned by the caller. On acceptance it moves
    // to OSAL, and is returned in exactly one completion. No borrowed storage.
    [[nodiscard]] Result<IoId> submit_read(PipeReader&, IoBuffer&&, Deadline);
    [[nodiscard]] Result<IoId> submit_write(PipeWriter&, IoBuffer&&, Deadline);
    [[nodiscard]] Result<CancelResult> cancel(IoId);
    // Cancelling this wait does not cancel I/O. Output slots receive ownership.
    [[nodiscard]] Result<std::size_t> wait_completions(std::span<IoCompletion>, Deadline,
                                                       std::stop_token = {});
    [[nodiscard]] Result<void> shutdown_until(Deadline, std::stop_token = {});
    std::optional<OsError> monitor_error() const;

private:
    friend struct detail::PipeAccess;
    PipeService(std::shared_ptr<detail::PipeState>, std::unique_ptr<detail::PipeWorker>);
    Result<IoId> submit(detail::PipeEnd&, IoBuffer&&, Deadline, IoKind);
    void close() noexcept;
    std::shared_ptr<detail::PipeState> state_;
    std::unique_ptr<detail::PipeWorker> worker_;
};
} // namespace vinc::osal

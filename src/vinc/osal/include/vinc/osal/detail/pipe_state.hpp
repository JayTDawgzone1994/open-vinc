#pragma once

// Private Pipe ownership graph shared by the scheduler and process-handoff adapter.
// Keep records alive while operations or an in-progress spawn still refer to their native endpoints.

#include <vinc/osal/error.hpp>
#include <vinc/time.hpp>
#include <vinc/osal/detail/pipe_native.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace vinc::osal::detail {
struct PipeRecord {
    PipeId id;
    std::unique_ptr<NativePipe> native;
    bool starting = true;
    bool closing = false;
    bool transferring = false;
    bool used = false;
    IoKind direction = IoKind::Read;
};

struct PipeOperationState {
    IoId id;
    std::shared_ptr<PipeRecord> pipe;
    IoKind kind;
    IoBuffer buffer;
    vinc::Deadline deadline;
    NativeTransfer native;
    bool started = false;
    bool done = false;
    bool cancellation_sent = false;
    std::optional<IoStatus> stop_reason;
    NativeIoResult result{IoStatus::Failed};
};

struct PipeState {
    std::mutex mutex;
    std::condition_variable_any changed;
    PipeServiceOptions options;
    std::unique_ptr<PipeMonitor> monitor;
    std::vector<std::shared_ptr<PipeRecord>> pipes;
    std::vector<std::unique_ptr<PipeOperationState>> operations;
    std::size_t buffered_bytes = 0;
    bool accepting = true;
    bool worker_done = false;
    bool abandoned = false;
    std::optional<vinc::Error> monitor_error;
};

struct PipeWorker {
    std::mutex join_mutex;
    std::thread thread;

    ~PipeWorker() {
        if (thread.joinable()) {
            thread.detach();
        }
    }
};

struct PipeAccess {
    static std::shared_ptr<PipeState> state(PipeService& service) {
        return service.state_;
    }

    static PipeEnd& end(PipeReader& reader) {
        return reader.end_;
    }

    static PipeEnd& end(PipeWriter& writer) {
        return writer.end_;
    }

    static std::shared_ptr<PipeState> state(PipeEnd& endpoint) {
        return endpoint.state_;
    }

    static std::shared_ptr<PipeRecord> record(PipeEnd& endpoint) {
        return endpoint.record_;
    }

    static vinc::Result<std::vector<std::shared_ptr<PipeRecord>>> reserve(PipeService&, std::size_t);
    static void rollback(PipeService&, std::span<const std::shared_ptr<PipeRecord>>) noexcept;
    static PipeEnd commit(PipeService&, const std::shared_ptr<PipeRecord>&, std::unique_ptr<NativePipe>,
                          IoKind);

    static PipeReader reader(PipeEnd endpoint) {
        PipeReader reader;
        reader.end_ = std::move(endpoint);
        return reader;
    }

    static PipeWriter writer(PipeEnd endpoint) {
        PipeWriter writer;
        writer.end_ = std::move(endpoint);
        return writer;
    }

    static void discard(PipeEnd&) noexcept;
};
}

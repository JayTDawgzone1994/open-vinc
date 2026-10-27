// Exercise the real scheduler with a deterministic native boundary, as in
// process_fault_tests.cpp. No production hooks or platform resources are used.
#include "../src/pipe.cpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vinc::osal;
using namespace std::chrono_literals;
constexpr auto test_timeout = 5s;
constexpr int rejected_attempts = 3;
constexpr std::size_t buffer_size = 7;
bool reject_attach = true;
vinc::NativeError injected_native;
std::atomic<int> transfers_started{0};

vinc::Deadline soon() {
    return std::chrono::steady_clock::now() + test_timeout;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
    return std::move(*result);
}
}

namespace vinc::osal::detail {
struct PipeMonitorImpl {
    std::mutex mutex;
    std::condition_variable changed;
    bool notified = false;
};
struct NativePipeImpl {};
struct NativeTransferImpl {
    std::optional<NativeIoResult> result;
};

PipeMonitor::PipeMonitor(std::unique_ptr<PipeMonitorImpl> impl) : impl_(std::move(impl)) {
}
PipeMonitor::~PipeMonitor() = default;

vinc::Result<std::unique_ptr<PipeMonitor>> PipeMonitor::create() {
    return std::unique_ptr<PipeMonitor>(new PipeMonitor(std::make_unique<PipeMonitorImpl>()));
}

void PipeMonitor::wake() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->notified = true;
    impl_->changed.notify_one();
}

vinc::Result<void> PipeMonitor::wait(int milliseconds) noexcept {
    std::unique_lock lock(impl_->mutex);
    const auto ready = [&] { return impl_->notified; };
    if (milliseconds < 0) {
        impl_->changed.wait(lock, ready);
    } else {
        impl_->changed.wait_for(lock, std::chrono::milliseconds(milliseconds), ready);
    }
    impl_->notified = false;
    return {};
}

vinc::Result<void> PipeMonitor::initialize_worker() noexcept {
    return {};
}

NativePipe::NativePipe(std::unique_ptr<NativePipeImpl> impl) : impl_(std::move(impl)) {
}
NativePipe::~NativePipe() = default;

vinc::Result<NativePipePair> NativePipe::create_pair(bool) {
    return NativePipePair{
        std::unique_ptr<NativePipe>(new NativePipe(std::make_unique<NativePipeImpl>())),
        std::unique_ptr<NativePipe>(new NativePipe(std::make_unique<NativePipeImpl>()))};
}

vinc::Result<void> NativePipe::attach(PipeMonitor&) {
    if (reject_attach) {
        return std::unexpected(vinc::Error{ErrorCode::SystemError, ErrorOperation::AdoptPipe,
                                           injected_native});
    }
    return {};
}

NativeTransfer::NativeTransfer() : impl_(std::make_unique<NativeTransferImpl>()) {
}
NativeTransfer::~NativeTransfer() = default;

void NativeTransfer::start(NativePipe&, PipeMonitor&, std::span<std::byte> buffer, IoKind) noexcept {
    ++transfers_started;
    impl_->result = NativeIoResult{IoStatus::Completed, buffer.size(), {}};
}

std::optional<NativeIoResult> NativeTransfer::poll() noexcept {
    return impl_->result;
}

void NativeTransfer::cancel() noexcept {
    impl_->result = NativeIoResult{IoStatus::Cancelled, 0, {}};
}
}

namespace {
void submission_contract(IoKind kind, vinc::NativeError native) {
    reject_attach = true;
    injected_native = native;
    transfers_started = 0;
    IoBuffer buffer(buffer_size);
    std::fill(buffer.bytes().begin(), buffer.bytes().end(), std::byte{0x5a});
    const auto* original = buffer.bytes().data();
    const auto capacity = buffer.capacity();
    PipeServiceOptions options;
    options.max_endpoints = 2;
    options.max_operations = 1;
    options.max_buffered_bytes = capacity;
    auto service = take(PipeService::create(options));
    auto pipe = take(service.create_pipe());
    const auto operation = kind == IoKind::Read ? ErrorOperation::ReadPipe : ErrorOperation::WritePipe;
    const auto pipe_id = kind == IoKind::Read ? pipe.reader.id() : pipe.writer.id();
    const auto submit = [&] {
        if (kind == IoKind::Read) {
            return service.submit_read(pipe.reader, std::move(buffer), soon());
        }
        return service.submit_write(pipe.writer, std::move(buffer), soon());
    };

    for (int attempt = 0; attempt < rejected_attempts; ++attempt) {
        auto rejected = submit();
        require(!rejected, "attach failure accepted an operation");
        const auto& error = rejected.error();
        require(error.is(ErrorCode::SystemError) && error.is_operation(operation) && error.native == native,
                "submission lost public operation or native diagnostic");
        require(error.causes().size() == 1 && !error.truncated(), "attach cause lost or duplicated");
        const auto expected = vinc::Error{ErrorCode::SystemError, ErrorOperation::AdoptPipe, native};
        require(error.causes().front() == expected.record(), "attach cause changed");
        require(buffer.bytes().data() == original && buffer.size() == buffer_size &&
                    buffer.capacity() == capacity &&
                    std::all_of(buffer.bytes().begin(), buffer.bytes().end(),
                                [](std::byte byte) { return byte == std::byte{0x5a}; }),
                "rejected submission consumed or modified caller buffer");
        require(transfers_started == 0, "unprotected native transfer started");
        std::array<IoCompletion, 1> output;
        auto waited = service.wait_completions(output, vinc::Deadline::min());
        require(!waited && waited.error().is(ErrorCode::Timeout), "rejected submission produced completion");
    }

    reject_attach = false;
    const auto id = take(submit());
    std::array<IoCompletion, 2> output;
    require(take(service.wait_completions(output, soon())) == 1, "retry did not return exactly one completion");
    const auto& completion = output[0];
    require(completion.id == id && completion.pipe == pipe_id && completion.kind == kind &&
                completion.status == IoStatus::Completed && completion.transferred == buffer_size &&
                completion.buffer.bytes().data() == original && transfers_started == 1,
            "retry lost buffer, capacity or identity");
    auto extra = service.wait_completions(output, vinc::Deadline::min());
    require(!extra && extra.error().is(ErrorCode::Timeout), "duplicate retry completion");
    require(!service.monitor_error(), "endpoint failure poisoned the service");
    require(static_cast<bool>(service.shutdown_until(soon())), "shutdown failed");
}
}

int main() {
    try {
        // Provenance is opaque to the common scheduler, on every target OS.
        for (auto native : {vinc::NativeError{vinc::NativeErrorSource::LinuxErrno, 13},
                            vinc::NativeError{vinc::NativeErrorSource::DarwinErrno, 13},
                            vinc::NativeError{vinc::NativeErrorSource::Win32, 5}}) {
            submission_contract(IoKind::Read, native);
            submission_contract(IoKind::Write, native);
        }
        std::cout << "PASS: attach failure wrapping, buffer retention, no I/O and retry without leaked credits\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

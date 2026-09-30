#include "native.hpp"
#include "process_pipe_native.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

namespace vinc::osal::detail {

struct ProcessRecord {
    ProcessId id;
    std::uint64_t pid = 0;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unique_ptr<NativeProcess> native;
    std::optional<ExitStatus> exit;
    std::optional<OsError> error;
    bool starting = true;
};

struct ServiceState {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::vector<std::shared_ptr<ProcessRecord>> records;
    std::unique_ptr<NativeMonitor> monitor;
    std::size_t capacity;
    bool accepting = true;
    bool worker_done = false;
    std::optional<OsError> monitor_error;
};

bool valid_utf8(const std::string& text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first == 0) return false;
        if (first < 0x80) continue;
        unsigned count;
        std::uint32_t code;
        std::uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 7; minimum = 0x10000; }
        else return false;
        if (text.size() - i < count) return false;
        while (count-- != 0) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

Result<void> validate_options(const SpawnOptions& options) {
    const auto valid_path = [](const std::filesystem::path& path) {
        return path.is_absolute() && path.native().find(typename std::filesystem::path::value_type{}) == path.native().npos;
    };
    if (!valid_path(options.executable) ||
        (options.working_directory && !valid_path(*options.working_directory)))
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Spawn});
    for (const auto& argument : options.arguments)
        if (!valid_utf8(argument)) return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Spawn});
    for (const auto& entry : options.environment) {
        // Reserved bootstrap is supplied only by OSAL's spawn integration.
        auto name = entry.name;
        for (char& ch : name) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        if (name == pipe_bootstrap_name)
            return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Spawn});
        if (entry.name.empty() || entry.name.find('=') != std::string::npos || !valid_utf8(entry.name) ||
            (entry.value && !valid_utf8(*entry.value)))
            return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Spawn});
    }
    return {};
}

// Requires record.mutex. This is the only native observation/reaping path.
void observe(ProcessRecord& record) noexcept {
    if (!record.native || record.exit || record.error) return;
    auto result = record.native->poll();
    if (!result) record.error = result.error();
    else if (*result) record.exit = **result;
    else return;
    record.native.reset();
    record.changed.notify_all();
}

void run_monitor(const std::shared_ptr<ServiceState>& state) noexcept {
    bool polling = false;
    for (;;) {
        {
            std::lock_guard lock(state->mutex);
            bool active = false;
            polling = state->monitor_error.has_value();
            for (const auto& record : state->records) {
                std::lock_guard record_lock(record->mutex);
                observe(*record);
                active |= record->starting || static_cast<bool>(record->native);
                if (record->native) polling |= !record->native->event_driven();
            }
            state->changed.notify_all();
            if (!state->accepting && !active) {
                state->worker_done = true;
                state->changed.notify_all();
                return;
            }
        }
        auto result = state->monitor->wait(polling);
        if (!result) {
            { std::lock_guard lock(state->mutex); state->monitor_error = result.error(); }
            // Broken native event infrastructure must not cause a hot loop or
            // abandon owned children. Expose the error and continue reaping.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

Result<ProcessId> next_id() noexcept {
    static std::atomic<std::uint64_t> sequence{1};
    auto value = sequence.load(std::memory_order_relaxed);
    do {
        if (value == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected(OsError{ErrorCode::ResourceExhausted, Operation::Spawn});
    } while (!sequence.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return ProcessId{value};
}

} // namespace vinc::osal::detail

namespace vinc::osal {

Process::Process(std::shared_ptr<detail::ServiceState> service, std::shared_ptr<detail::ProcessRecord> record)
    : service_(std::move(service)), record_(std::move(record)) {}
Process::operator bool() const noexcept { return static_cast<bool>(record_); }
ProcessId Process::id() const noexcept { return record_ ? record_->id : ProcessId{}; }
std::uint64_t Process::pid() const noexcept { return record_ ? record_->pid : 0; }

Result<std::optional<ExitStatus>> Process::try_wait() {
    if (!record_) return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Observe});
    std::lock_guard lock(record_->mutex);
    detail::observe(*record_);
    service_->monitor->wake();
    if (record_->error) return std::unexpected(*record_->error);
    return record_->exit;
}

Result<ExitStatus> Process::wait_until(Deadline deadline, std::stop_token cancellation) {
    if (!record_) return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Wait});
    std::unique_lock lock(record_->mutex);
    detail::observe(*record_);
    service_->monitor->wake();
    const auto finished = [&] { return record_->exit || record_->error; };
    if (!record_->changed.wait_until(lock, cancellation, deadline, finished))
        return std::unexpected(OsError{cancellation.stop_requested() ? ErrorCode::Cancelled : ErrorCode::Timeout, Operation::Wait});
    if (record_->error) return std::unexpected(*record_->error);
    return *record_->exit;
}

Result<TerminateResult> Process::terminate() {
    if (!record_) return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Terminate});
    std::lock_guard lock(record_->mutex);
    detail::observe(*record_);
    if (record_->error) return std::unexpected(*record_->error);
    if (record_->exit) return TerminateResult::AlreadyExited;
    auto result = record_->native->terminate();
    service_->monitor->wake();
    return result;
}

ProcessService::ProcessService(std::shared_ptr<detail::ServiceState> state) : state_(std::move(state)) {}
ProcessService::ProcessService(ProcessService&&) noexcept = default;
ProcessService& ProcessService::operator=(ProcessService&& other) noexcept {
    if (this != &other) { close_admission(); state_ = std::move(other.state_); }
    return *this;
}
ProcessService::~ProcessService() { close_admission(); }
void ProcessService::close_admission() noexcept {
    if (!state_) return;
    { std::lock_guard lock(state_->mutex); state_->accepting = false; }
    state_->monitor->wake();
}

Result<ProcessService> ProcessService::create(ServiceOptions options) {
    if (options.max_records == 0)
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::InitializeService});
    auto monitor = detail::NativeMonitor::create();
    if (!monitor) return std::unexpected(monitor.error());
    auto state = std::make_shared<detail::ServiceState>();
    state->capacity = options.max_records;
    state->records.reserve(options.max_records);
    state->monitor = std::move(*monitor);
    try {
        // The shared state stays alive until the last owned child is reaped,
        // including when the facade or all Process views are destroyed first.
        std::thread([state] { detail::run_monitor(state); }).detach();
    } catch (const std::system_error& error) {
        return std::unexpected(OsError{ErrorCode::ResourceExhausted, Operation::InitializeService, error.code()});
    }
    return ProcessService(std::move(state));
}

Result<Process> ProcessService::spawn(const SpawnOptions& options) {
    return spawn_impl(options, nullptr);
}
Result<Process> ProcessService::spawn_impl(const SpawnOptions& options, const detail::ChildPipe* child_pipe) {
    if (!state_) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::Spawn});
    auto valid = detail::validate_options(options);
    if (!valid) return std::unexpected(valid.error());
    auto id = detail::next_id();
    if (!id) return std::unexpected(id.error());
    auto record = std::make_shared<detail::ProcessRecord>();
    record->id = *id;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->accepting) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::Spawn});
        if (state_->records.size() == state_->capacity)
            return std::unexpected(OsError{ErrorCode::ResourceExhausted, Operation::Spawn});
        state_->records.push_back(record); // reserve ownership before OS creation
    }
    // Never hold the service lock across native spawn: existing children must
    // remain controllable while another executable is being launched.
    Result<std::unique_ptr<detail::NativeProcess>> native;
    try {
        native = detail::NativeProcess::spawn(options, *state_->monitor, child_pipe);
    } catch (...) {
        { std::lock_guard lock(state_->mutex); std::erase(state_->records, record); }
        state_->monitor->wake();
        throw;
    }
    {
        std::lock_guard lock(state_->mutex);
        if (!native) {
            std::erase(state_->records, record);
        } else {
            std::lock_guard record_lock(record->mutex);
            record->pid = (*native)->pid();
            record->native = std::move(*native);
            record->starting = false;
        }
    }
    state_->monitor->wake();
    if (!native) return std::unexpected(native.error());
    return Process(state_, std::move(record));
}

Result<Process> ProcessService::find(ProcessId id) const {
    if (!state_) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::Observe});
    std::lock_guard lock(state_->mutex);
    for (const auto& record : state_->records)
        if (record->id == id && !record->starting) return Process(state_, record);
    return std::unexpected(OsError{ErrorCode::NotFound, Operation::Observe});
}
Result<TerminateResult> ProcessService::terminate(ProcessId id) {
    auto process = find(id);
    if (!process) return std::unexpected(process.error());
    return process->terminate();
}
Result<void> ProcessService::forget(ProcessId id) {
    if (!state_) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::Forget});
    std::lock_guard lock(state_->mutex);
    const auto it = std::find_if(state_->records.begin(), state_->records.end(), [&](const auto& record) { return record->id == id; });
    if (it == state_->records.end()) return std::unexpected(OsError{ErrorCode::NotFound, Operation::Forget});
    auto record = *it;
    {
        std::lock_guard record_lock(record->mutex);
        detail::observe(*record);
        if (record->starting || record->native) return std::unexpected(OsError{ErrorCode::Busy, Operation::Forget});
    }
    state_->records.erase(it);
    state_->monitor->wake();
    return {};
}

Result<void> ProcessService::shutdown_until(Deadline deadline, std::stop_token cancellation) {
    if (!state_) return std::unexpected(OsError{ErrorCode::ServiceClosed, Operation::Shutdown});
    close_admission();
    std::unique_lock lock(state_->mutex);
    if (!state_->changed.wait_until(lock, cancellation, deadline, [&] { return state_->worker_done; }))
        return std::unexpected(OsError{cancellation.stop_requested() ? ErrorCode::Cancelled : ErrorCode::Timeout, Operation::Shutdown});
    for (const auto& record : state_->records) {
        std::lock_guard record_lock(record->mutex);
        if (record->error) return std::unexpected(*record->error);
    }
    return {};
}
std::optional<OsError> ProcessService::monitor_error() const {
    if (!state_) return std::nullopt;
    std::lock_guard lock(state_->mutex);
    return state_->monitor_error;
}

} // namespace vinc::osal

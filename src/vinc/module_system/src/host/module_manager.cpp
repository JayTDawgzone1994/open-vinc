#include <vinc/module/detail/python_launch.hpp>
#include <vinc/log/detail/manager.hpp>
#include <iostream>
#include <vinc/param/detail/manager.hpp>
#include <vinc/module/detail/instance.hpp>
#include <vinc/module/detail/lock_registry.hpp>
#include <vinc/module/detail/instance_name.hpp>
#include <vinc/module/detail/manager_access.hpp>
// Central module supervisor: allocate MIDs, approve names, launch through OSAL, wait for Ready,
// route requests, and stop/reap each generation before retiring its Message Bus resources.

#include <vinc/time.hpp>
#include <vinc/config.hpp>
#include <vinc/module/detail/contract_report.hpp>
#include <vinc/module/host/module_manager.hpp>
#include <vinc/module/detail/host_path.hpp>
#include <vinc/module/detail/mid_allocator.hpp>
#include <vinc/module/detail/module_protocol.hpp>
#include <vinc/message_bus/detail/bus_manager.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <vinc/osal/random.hpp>
#include <vinc/module/detail/error_protocol.hpp>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef VINC_MODULE_TESTING
#include "fault_hooks.hpp"
#endif

namespace vinc {
namespace {
using namespace std::chrono_literals;
using control::Clock;
using module_system::ErrorCode;
using module_system::ErrorOperation;
using vinc::Deadline;

// Per-record work budget prevents one busy child from monopolizing supervision.
constexpr int control_messages_per_pass = 16;
constexpr auto supervisor_poll_interval = 20ms;
constexpr auto terminate_retry_interval = 1s;
constexpr auto shutdown_observation_grace = 2s;
constexpr auto output_drain_timeout = 2s;
constexpr auto readiness_wait_grace = 1s;
constexpr auto adapter_command_timeout = 5s;
constexpr auto adapter_completion_timeout = 1s;
constexpr auto maximum_policy_timeout = 24h;
constexpr auto maximum_stop_timeout = 60s;
constexpr auto minimum_restart_delay = 100ms;
constexpr std::uint32_t maximum_restart_count = 1000;
constexpr std::size_t module_record_limit = 256;
constexpr std::size_t module_name_limit_bytes = 128;
constexpr int directory_reservation_attempts = 8;
constexpr std::size_t directory_nonce_characters = 12;
constexpr int module_listener_backlog = 4;
// One local Shell plus eight remote Shell subscribers.
constexpr std::size_t output_subscriber_limit = 10;

bool valid_policy(const ModulePolicy& policy) {
    const bool invalid_heartbeat = policy.heartbeat_interval <= 0ms ||
                                   policy.heartbeat_timeout <= policy.heartbeat_interval ||
                                   policy.heartbeat_timeout > maximum_policy_timeout;
    if (invalid_heartbeat) {
        return false;
    }

    const bool invalid_startup =
        policy.startup_timeout <= 0ms || policy.startup_timeout > maximum_policy_timeout;
    if (invalid_startup) {
        return false;
    }

    const bool invalid_stop = policy.stop_timeout <= 0ms || policy.stop_timeout > maximum_stop_timeout;
    if (invalid_stop) {
        return false;
    }

    return policy.restart_delay >= minimum_restart_delay && policy.max_restarts <= maximum_restart_count;
}

Error failure(ErrorCode code, ErrorOperation operation) {
    return Error{code, operation};
}

bool terminal(ModuleState state) {
    return state == ModuleState::Stopped || state == ModuleState::Failed;
}

std::string utf8(const std::filesystem::path& path) {
    const auto encoded_path = path.u8string();
    // Filesystem/UTF-8 boundary: char may inspect char8_t object bytes. Copy the
    // same UTF-8 code units into the control protocol string without transcoding.
    return {reinterpret_cast<const char*>(encoded_path.data()), encoded_path.size()};
}

Result<std::string> nonce() {
    constexpr std::size_t token_bytes = 16;
    std::array<std::byte, token_bytes> bytes;
    auto filled = osal::random_bytes(bytes);
    if (!filled) {
        return std::unexpected(
            wrap_error(ErrorCode::ResourcesUnavailable, ErrorOperation::ReserveDirectory, filled.error()));
    }

    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(token_bytes * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        result += hex[value >> 4];
        result += hex[value & 15];
    }
    return result;
}

std::atomic<MID> next_mid{1};

Result<MID> new_mid() {
    return detail::allocate_mid(next_mid);
}

// One logical Module across restarts. Record::mutex protects its current generation.
// Keep pending_request after a caller timeout: the child may still be executing it.
struct Record {
    std::mutex mutex;
    std::condition_variable changed;

    ModuleType module_type;
    ModuleSnapshot snapshot;
    std::vector<std::string> arguments;

    // Resources owned by the current process generation.
    std::optional<osal::Process> process;
    std::unique_ptr<detail::ContractReportStorage> contract_report;
    bool termination_requested = false;
    std::uint64_t output_capture = 0;
    std::optional<osal::Socket> listener;
    std::filesystem::path socket_path;
    std::shared_ptr<control::Session> session;
    std::string token;

    // Supervision continues independently of callers waiting for a result.
    vinc::Deadline startup_deadline{};
    vinc::Deadline stop_deadline{};
    vinc::Deadline restart_at{};
    vinc::Deadline next_terminate{};
    std::optional<Deadline> fatal_deadline;
    std::optional<Deadline> user_stop_deadline;
    bool fatal_deadline_expired = false;
    bool user_stop = false;
    bool finishing = false;
    bool completed = false;
    bool normal_completion = false;
    bool fault = false;
    bool stop_sent = false;

    bool debug_gate_seen = false;
    bool name_confirmed = false;
    bool startup_rejected = false;
    std::uint64_t last_fault_request = detail::name_registration_request;
    std::optional<control::Frame> name_request;

    // At most one command or completion request may be in flight.
    bool pending_waiter = false;
    std::uint64_t next_request = 1;
    std::uint64_t pending_request = 0;
    std::uint64_t pending_generation = 0;
    std::uint16_t pending_operation = 0;
    std::optional<control::Frame> response;
};

// Lives inside request()'s record lock, including exceptional exits. Once a
// request is queued, only its reply or confirmed exit may release Busy.
struct CommandWaiter {
    Record& record;

    ~CommandWaiter() {
        record.pending_waiter = false;
        if (record.response) {
            record.response.reset();
            record.pending_request = 0;
        }
    }
};

void retire_command(Record& record) {
    // A completed reply belongs to the old waiter, not to the next process.
    // pending_waiter blocks new requests until that waiter leaves its scope.
    if (!record.pending_waiter || !record.response) {
        record.pending_request = 0;
        record.response.reset();
    }
}

bool matches(const Record& record, const ModuleKey& key) {
    if (const auto* mid = std::get_if<MID>(&key)) {
        return *mid && record.snapshot.mid == *mid;
    }

    const auto& name = std::get<std::string>(key);
    return !name.empty() && record.snapshot.name == name;
}

// The first failure for one MID remains primary; cleanup cannot overwrite it.
void save_failure(Record& record, Error error) {
    if (!record.snapshot.last_failure || record.snapshot.last_failure->mid != record.snapshot.mid) {
        record.snapshot.last_failure = ModuleDiagnostic{record.snapshot.mid, error};
    }
}

void save_cleanup(Record& record, Error error) {
    if (!record.snapshot.cleanup_failure || record.snapshot.cleanup_failure->mid != record.snapshot.mid) {
        record.snapshot.cleanup_failure = ModuleDiagnostic{record.snapshot.mid, error};
    }
}

void close_listener(Record& record) {
    record.listener.reset();
    if (!record.socket_path.empty()) {
        std::error_code remove_error;
        std::filesystem::remove(record.socket_path, remove_error);
        if (remove_error) {
            save_cleanup(record, Error{ErrorCode::CleanupFailed, ErrorOperation::CloseEndpoint,
                                       osal::native_error(remove_error)});
        } else {
            record.socket_path.clear();
        }
    }
}

void record_failure(Record& record, Error error) {
    if (record.fatal_deadline) {
        // Secondary channel/observer failures cannot consume the fatal writer's
        // grace. Keep them separate from the original failure and its deadline.
        save_cleanup(record, error);
        return;
    }

    save_failure(record, error);
    const auto now = Clock::now();
    if (record.snapshot.state != ModuleState::Stopping) {
        record.stop_deadline = now;
    } else {
        record.stop_deadline = (std::min)(record.stop_deadline, now);
    }
    record.fault = true;
    record.snapshot.state = ModuleState::Stopping;
    record.changed.notify_all();
}

void request_user_stop(Record& record) {
    if (!record.user_stop) {
        record.user_stop_deadline = Clock::now() + record.module_type.policy.stop_timeout;
    }
    record.user_stop = true;
    if (record.fatal_deadline && record.user_stop_deadline) {
        record.stop_deadline = (std::min)(record.stop_deadline, *record.user_stop_deadline);
    }
    record.changed.notify_all();
}

void observe_contract_failure(Record& record, Deadline now) {
    if (!record.process || !record.contract_report || record.fatal_deadline) {
        return;
    }

    const auto observed = record.contract_report->observe_state();
    if (observed && *observed == detail::ContractReportState::Empty) {
        return;
    }

    auto deadline = record.snapshot.debug ? (Deadline::max)() : now + config::module_fatal_exit_timeout;
    if (record.snapshot.state == ModuleState::Starting) {
        deadline = (std::min)(deadline, record.startup_deadline);
    }
    if (record.snapshot.state == ModuleState::Stopping) {
        deadline = (std::min)(deadline, record.stop_deadline);
    }
    if (record.user_stop_deadline) {
        deadline = (std::min)(deadline, *record.user_stop_deadline);
    }

    record.fatal_deadline = deadline;
    record.stop_deadline = deadline;
    record.fault = true;
    record.snapshot.state = ModuleState::Stopping;
    if (observed) {
        save_failure(record, failure(ErrorCode::ContractViolation, ErrorOperation::ObserveFaultReport));
    } else {
        save_failure(record, observed.error());
    }
    record.changed.notify_all();
}

void preserve_termination_result(Record& record, const Result<osal::TerminateResult>& result,
                                 Deadline now) {
    record.next_terminate = now + terminate_retry_interval;
    if (!result) {
        save_cleanup(record, wrap_error(ErrorCode::TerminateFailed, ErrorOperation::Terminate,
                                        result.error()));
        return;
    }

    if (*result == osal::TerminateResult::Requested) {
        record.termination_requested = true;
    }
}

void terminate_if_due(Record& record, Deadline now) {
    if (!record.process || now < record.stop_deadline) {
        return;
    }
    if (record.fatal_deadline) {
        record.fatal_deadline_expired = true;
    }
    if (now < record.next_terminate) {
        return;
    }

#ifdef VINC_MODULE_TESTING
    const auto terminated = module_system::test::terminate_process(*record.process);
#else
    const auto terminated = record.process->terminate();
#endif
    preserve_termination_result(record, terminated, now);
}

// Each bundle owns partially acquired services until the manager takes ownership.
// Failure unwinds only local owners; no module has been admitted at this stage.
struct ProcessServices {
    std::unique_ptr<control::Service> channels;
    osal::ProcessService processes;
};

Result<ProcessServices> create_process_services() {
    auto channels = control::Service::create(control::detail::default_session_capacity * 2);
    if (!channels) {
        return std::unexpected(
            wrap_error(ErrorCode::ServiceFailed, ErrorOperation::Create, channels.error()));
    }
    auto processes = osal::ProcessService::create();
    if (!processes) {
        return std::unexpected(
            wrap_error(ErrorCode::ServiceFailed, ErrorOperation::Create, processes.error()));
    }
    return ProcessServices{std::move(*channels), std::move(*processes)};
}

struct DataServices {
    std::unique_ptr<output_hub::OutputHub> output;
    std::unique_ptr<message_bus::detail::BusManager> bus;
};

Result<DataServices> create_data_services(message_bus::Limits limits, std::shared_ptr<detail::ResourceNames> resources) {
    auto output =
        output_hub::OutputHub::create(output_hub::Limits{.max_subscribers = output_subscriber_limit});
    if (!output) {
        return std::unexpected(wrap_error(ErrorCode::OutputFailed, ErrorOperation::Create, output.error()));
    }
    auto bus = message_bus::detail::BusManager::create(limits, std::move(resources));
    if (!bus) {
        return std::unexpected(wrap_error(ErrorCode::BusFailed, ErrorOperation::Create, bus.error()));
    }
    return DataServices{std::move(*output), std::move(*bus)};
}

} // namespace

namespace {
Result<std::unique_ptr<param::detail::ParamManager>> create_param_services(
    detail::LockRegistry& locks, std::shared_ptr<detail::ResourceNames> names) {
    auto descriptor = locks.create();
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    auto storage = param::detail::Storage::create(std::move(names), descriptor->name, descriptor->id);
    if (!storage) {
        return std::unexpected(storage.error());
    }
    return param::detail::ParamManager::create(std::move(*storage));
}
}

struct ModuleManager::Impl {
    // Declared first, destroyed last: admission survives all resource producers.
    std::shared_ptr<detail::Instance> instance;
    std::unique_ptr<detail::LockRegistry> locks;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::map<std::string, ModuleType> module_types;
    std::vector<std::shared_ptr<Record>> records;
    std::map<std::string, microProgram> programs;
    std::set<std::string> reserved_commands;
    std::unique_ptr<control::Service> channels;
    osal::ProcessService processes;
    // One local Shell plus eight connected remote Shell subscribers.
    std::unique_ptr<output_hub::OutputHub> output;
    std::unique_ptr<message_bus::detail::BusManager> bus_manager;
    std::unique_ptr<param::detail::ParamManager> params;
    std::unique_ptr<log::detail::Manager> logs;
    std::string instance_name;
    std::filesystem::path directory;
    std::filesystem::path host_executable;
    std::jthread worker;
    bool closing = false;
    std::atomic<std::uint64_t> contract_revision{0};

    Impl(std::unique_ptr<control::Service> channel_service, osal::ProcessService process_service,
         std::unique_ptr<output_hub::OutputHub> output_service,
         std::unique_ptr<message_bus::detail::BusManager> bus_service)
        : channels(std::move(channel_service)), processes(std::move(process_service)),
          output(std::move(output_service)), bus_manager(std::move(bus_service)) {
    }

    ~Impl() {
        // Also rolls back a directory reserved before worker creation failed.
        std::error_code ignored;
        if (!directory.empty()) {
            std::filesystem::remove(directory, ignored);
        }
    }

    Result<void> reserve_socket_directory() {
        std::error_code native;
        const auto parent = std::filesystem::temp_directory_path(native);
        if (native) {
            return std::unexpected(Error{ErrorCode::EndpointFailed, ErrorOperation::ReserveDirectory,
                                         osal::native_error(native)});
        }

        for (int attempt = 0; attempt < directory_reservation_attempts; ++attempt) {
            auto token = nonce();
            if (!token) {
                return std::unexpected(token.error());
            }

            const auto candidate = parent / ("ov-" + token->substr(0, directory_nonce_characters));
            if (std::filesystem::create_directory(candidate, native)) {
                directory = candidate;
                break;
            }
            if (native) {
                return std::unexpected(Error{ErrorCode::EndpointFailed, ErrorOperation::ReserveDirectory,
                                             osal::native_error(native)});
            }
        }
        if (directory.empty()) {
            return std::unexpected(failure(ErrorCode::CapacityExceeded, ErrorOperation::ReserveDirectory));
        }
#ifndef _WIN32
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, native);
        if (native) {
            return std::unexpected(Error{ErrorCode::EndpointFailed, ErrorOperation::ReserveDirectory,
                                         osal::native_error(native)});
        }
#endif
        return {};
    }

    // Registry lock precedes record locks. The monitor releases a record before
    // entering name registration, so no record -> registry lock cycle exists.
    std::shared_ptr<Record> find_locked(const ModuleKey& key) const {
        for (auto& record : records) {
            std::lock_guard record_lock(record->mutex);
            if (matches(*record, key)) {
                return record;
            }
        }
        return {};
    }

    std::shared_ptr<Record> find(const ModuleKey& key) const {
        std::lock_guard registry_lock(mutex);
        return find_locked(key);
    }

    std::vector<std::shared_ptr<Record>> snapshot_records() const {
        std::lock_guard registry_lock(mutex);
        return records;
    }

    // All launch helpers run under Record::mutex. Partial acquisitions stay in
    // Record and are released by finished(); failed launch never abandons supervision.
    Result<void> reset_generation(Record& record) {
        // The first attempt already owns its MID before spawn; every retry gets another.
        if (record.snapshot.restarts) {
            auto mid = new_mid();
            if (!mid) {
                record.startup_rejected = true;
                return std::unexpected(mid.error());
            }
            record.snapshot.mid = *mid;
        }
        record.snapshot.generation =
            record.snapshot.mid; // Existing wire/Message Bus generation uses this same identity.
        record.name_confirmed = false;
        record.name_request.reset();
        record.snapshot.pid = 0;
        record.termination_requested = false;
        record.fault = false;
        record.finishing = false;
        record.completed = false;
        record.normal_completion = false;
        record.stop_sent = false;
        record.next_terminate = {};
        record.fatal_deadline.reset();
        record.user_stop_deadline.reset();
        record.fatal_deadline_expired = false;
        retire_command(record);
        record.last_fault_request = detail::name_registration_request;
        auto token = nonce();
        if (!token) {
            return std::unexpected(token.error());
        }
        record.token = std::move(*token);
        return {};
    }

    bool prepare_listener(Record& record) {
        // Prepare the control endpoint before starting the child.
        record.socket_path = directory / (std::to_string(record.snapshot.generation) + ".sock");
        auto listener = osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream);
        if (!listener) {
            record_failure(record, wrap_error(ErrorCode::EndpointFailed, ErrorOperation::OpenListener,
                                              listener.error()));
            return false;
        }

        record.listener.emplace(std::move(*listener));

        auto bound = record.listener->bind(osal::LocalEndpoint{utf8(record.socket_path)});
        if (!bound) {
            record_failure(
                record, wrap_error(ErrorCode::EndpointFailed, ErrorOperation::BindListener, bound.error()));
            return false;
        }

        auto listening = record.listener->listen(module_listener_backlog);
        if (!listening) {
            record_failure(record,
                           wrap_error(ErrorCode::EndpointFailed, ErrorOperation::Listen, listening.error()));
            return false;
        }

        if (logs) {
            control::Options options;
            options.generation = record.snapshot.mid;
            options.token = record.token;
            options.heartbeat_timeout_enabled = !record.snapshot.debug;
            options.heartbeat_interval = record.module_type.policy.heartbeat_interval;
            options.heartbeat_timeout = record.module_type.policy.heartbeat_timeout;
            options.handshake_timeout = record.module_type.policy.startup_timeout;
            auto log_listener = logs->listen(record.snapshot.mid, utf8(record.socket_path), options);
            if (!log_listener) {
                logs->diagnostic("Log listener unavailable: " + error_to_string(log_listener.error()));
            }
        }
        return true;
    }

    Result<osal::SpawnOptions> make_spawn_options(const Record& record) {
        const auto& policy = record.module_type.policy;
        osal::SpawnOptions options;
        // Central coordinates shutdown; terminal Ctrl+C must not kill children first.
        options.new_process_group = true;
        options.executable = host_executable;
        options.arguments = {detail::bootstrap::version_marker,
                             utf8(record.socket_path),
                             record.token,
                             std::to_string(record.snapshot.generation),
                             std::to_string(policy.heartbeat_interval.count()),
                             std::to_string(policy.heartbeat_timeout.count()),
                             std::to_string(policy.startup_timeout.count()),
                             std::to_string(policy.stop_timeout.count()),
                             record.module_type.name,
                             record.contract_report->name(),
                             utf8(record.module_type.python ? record.module_type.python->script : record.module_type.library),
                             record.snapshot.debug ? "1" : "0"};
        options.arguments.insert(options.arguments.end(), record.arguments.begin(), record.arguments.end());

        if (record.module_type.python) {
            auto prepared = detail::prepare_python_spawn(options, *record.module_type.python);
            if (!prepared) {
                return std::unexpected(prepared.error());
            }
        }
        return options;
    }

    void spawn_child(Record& record, const osal::SpawnOptions& options) {
        auto child =
            output->spawn(processes, options,
                          record.snapshot.name.empty() ? "starting:" + std::to_string(record.snapshot.mid)
                                                       : record.snapshot.name,
                          record.snapshot.generation);
        if (!child) {
            record_failure(record, wrap_error(ErrorCode::SpawnFailed, ErrorOperation::Spawn, child.error()));
            return;
        }

        record.output_capture = child->capture;
        record.process.emplace(std::move(child->process));
        record.snapshot.pid = record.process->pid();
        if (logs) {
            logs->identify(record.snapshot.mid,
                           record.snapshot.name.empty() ? "starting:" + std::to_string(record.snapshot.mid)
                                                        : record.snapshot.name,
                           record.snapshot.pid);
        }
        record.snapshot.state = ModuleState::Starting;
        record.changed.notify_all();
    }

    // Each generation gets a fresh MID, token and endpoint before resource acquisition.
    void launch(Record& record) {
        auto reset = reset_generation(record);
        if (!reset) {
            record_failure(record, reset.error());
            return;
        }
        if (!prepare_listener(record)) {
            return;
        }

        auto report_name = instance->reserve();
        if (!report_name) {
            record_failure(record, report_name.error());
            return;
        }
        auto report = detail::ContractReportStorage::create(*report_name, record.snapshot.mid);
        if (!report) {
            record_failure(record, report.error());
            return;
        }
        record.contract_report = std::move(*report);

        auto options = make_spawn_options(record);
        if (!options) {
            record_failure(record, options.error());
            return;
        }
        record.startup_deadline = record.snapshot.debug
                                      ? (Deadline::max)()
                                      : Clock::now() + record.module_type.policy.startup_timeout;
        spawn_child(record, *options);
    }

    // Release the completed run and apply restart policy to the same logical Module.
    // An explicit user stop suppresses restart even if retries remain.
    void finished(Record& record) {
        if (logs) {
            logs->exited(record.snapshot.mid);
            if (record.snapshot.last_failure && record.snapshot.last_failure->mid == record.snapshot.mid) {
                logs->diagnostic("Module stopped: " + record.snapshot.name +
                                     " mid=" + std::to_string(record.snapshot.mid) + " " +
                                     error_to_string(record.snapshot.last_failure->error),
                                 log::Level::Error);
            }
        }
        if (record.session) {
            record.session->close();
        }
        record.session.reset();
        close_listener(record);

        if (record.process) {
            auto forgotten = processes.forget(record.process->id());
            if (!forgotten) {
                save_cleanup(
                    record, wrap_error(ErrorCode::CleanupFailed, ErrorOperation::Cleanup, forgotten.error()));
            }
            record.process.reset();
        }
        if (record.contract_report) {
            auto closed = record.contract_report->close();
            if (!closed) {
                save_cleanup(record, closed.error());
            }
            if (closed) {
                auto retired = instance->retire(record.contract_report->name());
                if (!retired) {
                    save_cleanup(record, retired.error());
                }
            }
            record.contract_report.reset();
        }
        retire_command(record);

        record.snapshot.debug_waiting = false;
        if (!record.snapshot.debug && !record.user_stop && !record.normal_completion && !record.startup_rejected &&
            record.snapshot.restarts < record.module_type.policy.max_restarts) {
            ++record.snapshot.restarts;
            record.restart_at = Clock::now() + record.module_type.policy.restart_delay;
            record.snapshot.state = ModuleState::RestartWaiting;
        } else {
            record.snapshot.state = (record.user_stop || record.normal_completion) ? ModuleState::Stopped : ModuleState::Failed;
        }
        record.changed.notify_all();
    }

    void collect_contract_report(Record& record, const osal::ExitStatus& exit) {
        if (!record.contract_report ||
            (record.snapshot.last_contract_failure &&
             record.snapshot.last_contract_failure->mid == record.snapshot.mid)) {
            return;
        }
        auto read = record.contract_report->read_after_exit();
        if (read && !*read && !record.fatal_deadline) {
            return;
        }

        ModuleContractDiagnostic diagnostic;
        diagnostic.mid = record.snapshot.mid;
        diagnostic.pid = record.snapshot.pid;
        diagnostic.exit = exit;
        diagnostic.termination_requested = record.termination_requested;
        diagnostic.exit_deadline_expired = record.fatal_deadline_expired;
        if (read && *read) {
            diagnostic.report = **read;
            save_failure(record, failure(ErrorCode::ContractViolation, ErrorOperation::ObserveExit));
        } else {
            // Once a fatal state was seen, an empty record cannot erase that
            // observation. Do not fabricate a payload from partially written bytes.
            const auto error = read ? failure(ErrorCode::IncompleteFaultReport, ErrorOperation::ReadFaultReport)
                                    : read.error();
            diagnostic.report = std::unexpected(error);
            save_cleanup(record, error);
        }
        // Separate from first-failure precedence: a channel close cannot hide
        // the later recovered root diagnostic. Copies here never allocate.
        diagnostic.sequence = contract_revision.load() + 1;
        record.snapshot.last_contract_failure = diagnostic;
        if (logs && diagnostic.report) {
            const auto& report = *diagnostic.report;
            auto text = "Module contract failure: " + record.snapshot.name +
                        " mid=" + std::to_string(record.snapshot.mid) +
                        " pid=" + std::to_string(record.snapshot.pid) + " reason=" + report.reason.data() +
                        " file=" + report.file.data() + ":" + std::to_string(report.line) +
                        " function=" + report.function.data();
            if (report.has_index) {
                text += " index=" + std::to_string(report.index) + " size=" + std::to_string(report.size);
            }
            logs->diagnostic(std::move(text), log::Level::Error);
        }
        contract_revision.store(diagnostic.sequence);
        record.fault = true;
    }

    // Advance one Module while its record mutex is held; poll for child readiness and exit.
    void tick(Record& record) {
        const auto now = Clock::now();
        if (terminal(record.snapshot.state)) {
            return;
        }

        if (record.user_stop && !record.process) {
            finished(record);
            return;
        }

        // Start queued work, or wait until a scheduled retry is due.
        if (record.snapshot.state == ModuleState::Queued ||
            (record.snapshot.state == ModuleState::RestartWaiting && now >= record.restart_at)) {
            launch(record);
        }
        if (record.snapshot.state == ModuleState::RestartWaiting) {
            return;
        }

        // Latch fatal before processing Ready/name messages or channel errors.
        observe_contract_failure(record, Clock::now());
        receive_control_frames(record);

        // Observe OS exit before retiring any resources shared with the child.
        if (record.process) {
            auto exit_status = record.process->try_wait();
            if (!exit_status) {
                record_failure(record, wrap_error(ErrorCode::ObserveFailed, ErrorOperation::ObserveExit,
                                                  exit_status.error()));
            } else if (*exit_status) {
                // The OSAL worker may have recovered between supervisor ticks.
                // Preserve its first diagnostic before forgetting the OS record.
                if (const auto observed = record.process->observation_error()) {
                    record_failure(record, wrap_error(ErrorCode::ObserveFailed, ErrorOperation::ObserveExit,
                                                      *observed));
                }
                record.snapshot.last_exit = **exit_status;
                collect_contract_report(record, **exit_status);
                output->exited(record.output_capture);

                // Only confirmed OS exit permits reclaiming this generation's shared reader leases.
                // A missing heartbeat alone does not prove that the process has stopped touching memory.
                auto recovered = locks->exited(record.snapshot.mid);
                if (!recovered) {
                    save_cleanup(record, recovered.error());
                }
                bus_manager->exited(record.snapshot.generation);
                params->exited(record.snapshot.mid);
                record.normal_completion = record.completed && !record.fault &&
                    (**exit_status).kind == osal::ExitKind::Code && (**exit_status).value == 0;
                if (!record.user_stop && !record.fault && !record.normal_completion) {
                    save_failure(record, failure(ErrorCode::ProcessExited, ErrorOperation::ObserveExit));
                }
                finished(record);
                return;
            }
        }
        if (record.fault && !record.process) {
            finished(record);
            return;
        }

        if (record.fatal_deadline) {
            // A contract failure bypasses user cleanup. Never send Stop to a
            // runtime whose application thread may already be irrecoverably stuck.
            terminate_if_due(record, now);
            return;
        }

        // Request graceful stop, then ask the OS to terminate after the deadline.
        if (record.user_stop || record.fault || record.finishing) {
            if (record.snapshot.state != ModuleState::Stopping) {
                record.snapshot.state = ModuleState::Stopping;
                record.stop_deadline = now + record.module_type.policy.stop_timeout;
                record.changed.notify_all();
            }
            if (!record.finishing && !record.stop_sent && record.session && record.session->state() == control::State::Open) {
                auto sent = record.session->send({control::Kind::Event, detail::lifecycle, detail::stop});
                record.stop_sent = bool(sent);
                if (!sent) {
                    save_cleanup(record,
                                 wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Stop, sent.error()));
                }
            }
            terminate_if_due(record, now);
            return;
        }

        // Accept the child's control connection once per process generation.
        if (record.listener && !record.session) {
            auto accepted = record.listener->try_accept();
            if (accepted) {
                control::Options options;
                options.heartbeat_timeout_enabled = !record.snapshot.debug;
                options.generation = record.snapshot.generation;
                options.token = record.token;
                options.heartbeat_interval = record.module_type.policy.heartbeat_interval;
                options.heartbeat_timeout = record.module_type.policy.heartbeat_timeout;
                options.handshake_timeout = record.module_type.policy.startup_timeout;
                auto attached = channels->attach(std::move(*accepted), options);
                if (!attached) {
                    record_failure(record, wrap_error(ErrorCode::ChannelFailed, ErrorOperation::AttachChannel,
                                                      attached.error()));
                    return;
                }

                record.session = *attached;
                close_listener(record);
            } else if (!(accepted.error().is(vinc::osal::ErrorCode::WouldBlock))) {
                record_failure(
                    record, wrap_error(ErrorCode::EndpointFailed, ErrorOperation::Accept, accepted.error()));
                return;
            }
        }

        if (record.snapshot.state == ModuleState::Starting && now >= record.startup_deadline) {
            record_failure(record, failure(ErrorCode::StartupTimeout, ErrorOperation::Ready));
        }
    }

    // Caller retains Record::mutex across the entire batch. Name claims are only
    // queued here; register_name() later takes registry -> record locks in that order.
    void receive_control_frames(Record& record) {
        // Bound each pass so a busy Module cannot monopolize the supervisor.
        if (record.session) {
            for (int messages_processed = 0; messages_processed < control_messages_per_pass;
                 ++messages_processed) {
                auto received = record.session->try_receive();
                if (!received) {
                    if (record.fatal_deadline || (!record.user_stop && !record.fault && !record.finishing)) {
                        record_failure(record, wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Receive,
                                                          received.error()));
                    }
                    break;
                }

                if (!*received) {
                    break;
                }

                auto frame = std::move(**received);
                const bool command_response =
                    frame.kind == control::Kind::Response && frame.domain == detail::commands;
                if (record.fatal_deadline && !command_response) {
                    // Drain bounded input without starting new work or accepting
                    // Ready. Replies already received still belong to their waiter.
                    continue;
                }
                if (frame.kind == control::Kind::Event && frame.domain == detail::lifecycle &&
                    frame.operation == detail::debug_waiting) {
                    if (!record.snapshot.debug || record.debug_gate_seen || !frame.payload.empty() ||
                        record.snapshot.state != ModuleState::Starting || record.name_confirmed) {
                        record_failure(record, failure(ErrorCode::Protocol, ErrorOperation::WaitDebugReady));
                        break;
                    }
                    record.debug_gate_seen = true;
                    record.snapshot.debug_waiting = true;
                    record.changed.notify_all();
                } else if (frame.kind == control::Kind::Request && frame.domain == detail::lifecycle &&
                           (frame.operation == detail::finishing || frame.operation == detail::completed)) {
                    receive_completion(record, frame);
                } else if (frame.kind == control::Kind::Request && frame.domain == detail::lifecycle &&
                    frame.operation == detail::report_failure) {
                    receive_failure(record, frame);
                } else if (frame.kind == control::Kind::Request && frame.domain == detail::lifecycle &&
                           frame.operation == detail::attach_lock) {
                    receive_lock_request(record, frame);
                } else if (frame.domain == param::detail::domain) {
                    if (!record.name_confirmed || record.fault || record.user_stop || record.finishing) {
                        record_failure(record, failure(ErrorCode::Protocol, ErrorOperation::AccessParam));
                        break;
                    }
                    const auto operation = frame.operation;
                    const auto request = frame.request;
                    auto submitted = params->submit(record.snapshot.mid, record.session, std::move(frame));
                    if (!submitted) {
                        if (submitted.error().is(param::ErrorCode::CapacityExceeded)) {
                            control::Frame response{control::Kind::Response, param::detail::domain, operation, request};
                            response.payload = param::detail::encode_failure(submitted.error());
                            (void)record.session->send(std::move(response));
                        } else {
                            record_failure(record, wrap_error(ErrorCode::ParamFailed, ErrorOperation::Receive,
                                                              submitted.error()));
                            break;
                        }
                    }
                } else if (frame.domain == message_bus::detail::message_bus_domain) {
                    if (!record.name_confirmed) {
                        record_failure(record, failure(ErrorCode::Protocol, ErrorOperation::Receive));
                        break;
                    }

                    auto submitted = bus_manager->submit(record.snapshot.generation, record.snapshot.name,
                                                         record.session, std::move(frame));
                    if (!submitted) {
                        record_failure(record, wrap_error(ErrorCode::BusFailed, ErrorOperation::Receive,
                                                          submitted.error()));
                        break;
                    }
                } else if (frame.kind == control::Kind::Request && frame.domain == detail::lifecycle &&
                           frame.operation == detail::register_name) {
                    if (record.name_confirmed || record.name_request ||
                        frame.request != detail::name_registration_request ||
                        record.snapshot.state != ModuleState::Starting) {
                        record_failure(record, failure(ErrorCode::Protocol, ErrorOperation::RegisterName));
                        break;
                    }

                    // Name registration needs the registry lock, after tick releases this record.
                    record.name_request = std::move(frame);
                } else if (frame.kind == control::Kind::Event && frame.domain == detail::lifecycle &&
                           frame.operation == detail::ready) {
                    control::Reader reader(frame.payload);
                    const auto name = reader.string();
                    if (!record.name_confirmed || name != record.snapshot.name || !reader.done() ||
                        record.snapshot.state != ModuleState::Starting) {
                        record_failure(record, failure(ErrorCode::Protocol, ErrorOperation::Ready));
                        break;
                    }

                    record.snapshot.state = ModuleState::Running;
                    record.changed.notify_all();
                } else if (frame.kind == control::Kind::Response && frame.domain == detail::commands) {
                    if (record.pending_request == frame.request &&
                        record.pending_operation == frame.operation) {
                        if (record.pending_waiter) {
                            // A response can only settle the request of this session.
                            if (record.pending_generation != record.snapshot.generation) {
                                continue;
                            }
                            record.response = std::move(frame);
#ifdef VINC_MODULE_TESTING
                            module_system::test::checkpoint(module_system::test::Point::ResponseSaved,
                                                            &*record.process);
#endif
                        } else {
                            record.pending_request = 0;
                        }
                        record.changed.notify_all();
                    } // A timed-out request stays busy until its reply or process exit.
                } else {
                    record_failure(record, failure(ErrorCode::Protocol, ErrorOperation::Receive));
                    break;
                }
            }
        }
    }

    void receive_lock_request(Record& record, const control::Frame& frame) {
        control::Reader reader(frame.payload);
        const auto version = reader.u32();
        const auto id = reader.u64();
        if (!reader.done() || version != detail::lock_protocol_version || !frame.request ||
            !record.name_confirmed) {
            record_failure(record, failure(ErrorCode::Protocol, ErrorOperation::AccessLock));
            return;
        }
        auto descriptor = (record.user_stop || record.fault || record.finishing)
            ? Result<detail::LockDescriptor>(std::unexpected(failure(ErrorCode::Closing, ErrorOperation::AccessLock)))
            : locks->descriptor(id);
        control::Frame response{control::Kind::Response, detail::lifecycle, detail::attach_lock, frame.request};
        if (!descriptor) {
            response.payload = detail::encode_failure(descriptor.error());
        } else {
            control::put_u32(response.payload, detail::result_success);
            control::put_u32(response.payload, detail::lock_protocol_version);
            control::put_u64(response.payload, descriptor->id);
            control::put_u64(response.payload, record.snapshot.mid);
            auto encoded = control::put_string(response.payload, descriptor->name);
            if (!encoded) {
                record_failure(record, wrap_error(ErrorCode::Protocol, ErrorOperation::AccessLock, encoded.error()));
                return;
            }
        }
        auto sent = record.session->send(std::move(response));
        if (!sent) {
            record_failure(record, wrap_error(ErrorCode::ChannelFailed, ErrorOperation::AccessLock, sent.error()));
        }
    }

    void receive_completion(Record& record, const control::Frame& frame) {
        const bool beginning = frame.operation == detail::finishing;
        const bool invalid_phase = beginning ? (record.finishing ||
            (record.snapshot.state != ModuleState::Running && !record.user_stop)) :
            (!record.finishing || record.completed);
        if (invalid_phase || frame.request != detail::completion_request || !frame.payload.empty()) {
            record_failure(record, failure(ErrorCode::Protocol, ErrorOperation::Finish));
            return;
        }
        if (beginning) {
            record.finishing = true;
            if (record.snapshot.state != ModuleState::Stopping) {
                record.stop_deadline = Clock::now() + record.module_type.policy.stop_timeout;
            }
            record.snapshot.state = ModuleState::Stopping;
        } else {
            record.completed = true;
        }
        // Completion is provisional until OS exit. Faults always keep precedence.
        auto acknowledged = record.session->send(
            {control::Kind::Response, detail::lifecycle, frame.operation, frame.request});
        if (!acknowledged) {
            record_failure(record, wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Finish,
                                              acknowledged.error()));
        }
        record.changed.notify_all();
    }

    void receive_failure(Record& record, const control::Frame& frame) {
        if (frame.request <= record.last_fault_request || frame.payload.size() < detail::result_tag_bytes) {
            record_failure(record, failure(ErrorCode::Protocol, ErrorOperation::ReportFailure));
            return;
        }

        control::Reader reader(frame.payload);
        const auto role = reader.u32();
        auto reported =
            detail::decode_error(std::span<const std::byte>(frame.payload).subspan(detail::result_tag_bytes));
        if (!reported || role > detail::cleanup_failure || reported->domain != ErrorDomain::ModuleSystem) {
            record_failure(record, failure(ErrorCode::Protocol, ErrorOperation::ReportFailure));
            return;
        }

        record.last_fault_request = frame.request;
        if (role == detail::cleanup_failure) {
            record.fault = true;
            save_cleanup(record, *reported);
        } else {
            save_failure(record, *reported);
            record.fault = true;
        }
        // Leave enough time for on_stop and the diagnostic acknowledgement to finish.
        if (record.snapshot.state != ModuleState::Stopping) {
            record.snapshot.state = ModuleState::Stopping;
            record.stop_deadline = Clock::now() + record.module_type.policy.stop_timeout;
        }
        auto sent = record.session->send(
            {control::Kind::Response, detail::lifecycle, detail::report_failure, frame.request});
        if (!sent) {
            save_cleanup(record,
                         wrap_error(ErrorCode::ChannelFailed, ErrorOperation::ReportFailure, sent.error()));
        }
        record.changed.notify_all();
    }

    void register_name(const std::shared_ptr<Record>& record) {
        std::lock_guard registry_lock(mutex);
        std::lock_guard record_lock(record->mutex);
        if (!record->name_request || record->fault || record->user_stop || closing) {
            return;
        }

        auto request = std::move(*record->name_request);
        record->name_request.reset();
        control::Reader reader(request.payload);
        auto name = reader.string();
        std::optional<Error> rejection;

        if (!reader.done() || name.empty() || name.size() > module_name_limit_bytes ||
            !param::valid_segment(name)) {
            rejection = failure(ErrorCode::InvalidName, ErrorOperation::RegisterName);
        } else if (!record->snapshot.name.empty() && name != record->snapshot.name) {
            rejection = failure(ErrorCode::RestartNameChanged, ErrorOperation::RegisterName);
        } else if (name == "vinc" || name == "param" || reserved_commands.contains(name) || module_types.contains(name) ||
                   programs.contains(name)) {
            rejection = failure(ErrorCode::NameReserved, ErrorOperation::RegisterName);
        }

        // A completed record may release its name; a live or restarting Module keeps it.
        std::shared_ptr<Record> previous;
        if (!rejection) {
            for (auto& other : records) {
                if (other == record) {
                    continue;
                }

                std::lock_guard other_lock(other->mutex);
                if (other->snapshot.name != name) {
                    continue;
                }

                if (terminal(other->snapshot.state) && !other->process) {
                    previous = other;
                } else {
                    rejection = failure(ErrorCode::NameExists, ErrorOperation::RegisterName);
                }
                break;
            }
        }

        control::Frame response{control::Kind::Response, detail::lifecycle, detail::register_name,
                                request.request};
        if (rejection) {
            response.payload = detail::encode_failure(*rejection);
        } else {
            control::put_u32(response.payload, detail::result_success);
            auto encoded = control::put_string(response.payload, name);
            if (!encoded) {
                record_failure(
                    *record, wrap_error(ErrorCode::Protocol, ErrorOperation::RegisterName, encoded.error()));
                return;
            }
        }
        if (!rejection) {
            // Publish the name while admission remains locked, before acknowledging it.
            output->set_name(record->output_capture, name);
            if (previous) {
                std::erase(records, previous);
            }
            record->snapshot.name = std::move(name);
            record->name_confirmed = true;
            params->admit(record->snapshot.mid, record->snapshot.name);
            if (logs) {
                logs->identify(record->snapshot.mid, record->snapshot.name, record->snapshot.pid);
            }
        } else {
            save_failure(*record, *rejection);
            record->startup_rejected = true;
            record->fault = true;
            record->snapshot.state = ModuleState::Stopping;
            record->stop_deadline = Clock::now() + record->module_type.policy.stop_timeout;
        }

        auto sent = record->session->send(std::move(response));
        if (!sent) {
            record_failure(*record, wrap_error(ErrorCode::ChannelFailed, ErrorOperation::Send, sent.error()));
        }
        record->changed.notify_all();
    }

    void monitor(std::stop_token token) {
        // Admission bounds the array. Copying shared owners here cannot allocate during a fault.
        std::array<std::shared_ptr<Record>, module_record_limit> current;
        while (!token.stop_requested()) {
            std::size_t count = 0;
            {
                std::lock_guard registry_lock(mutex);
                count = records.size();
                std::copy(records.begin(), records.end(), current.begin());
            }
            for (auto& record : std::span(current).first(count)) {
                try {
                    {
                        std::lock_guard record_lock(record->mutex);
                        tick(*record);
                    }
                    // Release the record before taking registry -> record locks.
                    register_name(record);
                } catch (const std::system_error& error) {
                    std::lock_guard record_lock(record->mutex);
                    record_failure(*record, Error{ErrorCode::SupervisorFailed, ErrorOperation::Supervise,
                                                  osal::native_error(error.code())});
                } catch (const std::bad_alloc&) {
                    std::lock_guard record_lock(record->mutex);
                    record_failure(*record, failure(ErrorCode::ResourceExhausted, ErrorOperation::Supervise));
                } catch (...) {
                    std::lock_guard record_lock(record->mutex);
                    record_failure(*record, failure(ErrorCode::SupervisorFailed, ErrorOperation::Supervise));
                }
                record.reset();
            }
            std::unique_lock registry_lock(mutex);
            wake.wait_for(registry_lock, supervisor_poll_interval);
        }
    }

    // Allow one outstanding command/completion per Module. Capture its generation before
    // waiting, and never replay the request if timeout, cancellation or restart loses the result.
    Result<control::Frame> request(const ModuleKey& key, std::uint16_t operation,
                                   std::vector<std::string> args, Deadline deadline,
                                   std::stop_token stop = {}) {
        const auto action = operation == detail::execute ? ErrorOperation::Command : ErrorOperation::Complete;
        if (stop.stop_requested()) {
            return std::unexpected(failure(ErrorCode::Cancelled, action));
        }

        auto record = find(key);
        if (!record) {
            return std::unexpected(failure(ErrorCode::ModuleNotFound, action));
        }
        auto payload = control::encode_arguments(args);
        if (!payload) {
            return std::unexpected(wrap_error(ErrorCode::InvalidArgument, action, payload.error()));
        }

        std::unique_lock record_lock(record->mutex);
        if (!matches(*record, key)) {
            return std::unexpected(failure(ErrorCode::GenerationChanged, action));
        }
        if (record->snapshot.state != ModuleState::Running || record->user_stop) {
            return std::unexpected(failure(ErrorCode::NotRunning, action));
        }
        if (record->pending_request || record->pending_waiter) {
            return std::unexpected(failure(ErrorCode::Busy, action));
        }
        if (Clock::now() >= deadline) {
            return std::unexpected(failure(ErrorCode::Timeout, action));
        }
        if (!record->next_request) {
            return std::unexpected(failure(ErrorCode::RequestIdExhausted, action));
        }

        const auto request_id = record->next_request++;
        const auto generation = record->snapshot.generation;
        record->pending_request = request_id;
        record->pending_generation = generation;
        record->pending_operation = operation;
        record->response.reset();
        record->pending_waiter = true;
        CommandWaiter waiter{*record};
        Result<void> sent;
        try {
#ifdef VINC_MODULE_TESTING
            module_system::test::checkpoint(module_system::test::Point::BeforeSend);
#endif
            sent = record->session->send(
                {control::Kind::Request, detail::commands, operation, request_id, std::move(*payload)});
        } catch (const std::bad_alloc&) {
            // Session::send allocates only before enqueue succeeds. Its queued
            // byte accounting and wake path do not allocate; nothing was sent.
            record->pending_request = 0;
            throw;
        }
        if (!sent) {
            record->pending_request = 0;
            record->pending_waiter = false;
            return std::unexpected(wrap_error(ErrorCode::ChannelFailed, action, sent.error()));
        }
#ifdef VINC_MODULE_TESTING
        module_system::test::checkpoint(module_system::test::Point::AfterSend);
#endif

        while (!record->response && !record->user_stop && record->snapshot.state == ModuleState::Running &&
               record->snapshot.generation == generation && !stop.stop_requested() &&
               Clock::now() < deadline) {
            record->changed.wait_until(record_lock,
                                       (std::min)(deadline, Clock::now() + supervisor_poll_interval));
        }
        // A received response settles the result even if shutdown was requested concurrently.
        if (record->response && record->response->request == request_id &&
            record->pending_generation == generation) {
            record->pending_request = 0;
            auto response = std::move(*record->response);
            record->response.reset();
            return response;
        }
        if (record->snapshot.generation != generation) {
            return std::unexpected(failure(ErrorCode::GenerationChangedOutcomeUnknown, action));
        }
        if (record->user_stop || record->snapshot.state != ModuleState::Running) {
            if (record->snapshot.last_failure && record->snapshot.last_failure->mid == generation) {
                return std::unexpected(wrap_error(ErrorCode::SessionEndedOutcomeUnknown, action,
                                                  record->snapshot.last_failure->error));
            }
            return std::unexpected(failure(ErrorCode::SessionEndedOutcomeUnknown, action));
        }
        if (stop.stop_requested()) {
            return std::unexpected(failure(ErrorCode::CancelledOutcomeUnknown, action));
        }
        // The supervisor retains Busy until a late response or confirmed process exit.
        return std::unexpected(failure(ErrorCode::TimeoutOutcomeUnknown, action));
    }
};

std::uint64_t ModuleManager::contract_diagnostic_revision() const noexcept {
    return impl_->contract_revision.load();
}

// Manager lifetime and shared services.

ModuleManager::ModuleManager(std::unique_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation)) {
}

Result<std::unique_ptr<ModuleManager>> ModuleManager::create(message_bus::Limits limits, std::filesystem::path host_executable) {
    return detail::ManagerAccess::create(std::string(detail::instance_name), limits, std::move(host_executable));
}

Result<std::unique_ptr<ModuleManager>> detail::ManagerAccess::create(std::string name, message_bus::Limits limits, std::filesystem::path host_executable) {
    try {
        if (host_executable.empty()) {
            host_executable = std::filesystem::path(VINC_MODULE_HOST_PATH);
        }
        if (!host_executable.is_absolute()) {
            return std::unexpected(failure(ErrorCode::InvalidArgument, ErrorOperation::Create));
        }
        auto instance = detail::Instance::create(name);
        if (!instance) {
            return std::unexpected(instance.error());
        }
        auto process_services = create_process_services();
        if (!process_services) {
            return std::unexpected(process_services.error());
        }
        auto data_services = create_data_services(limits, *instance);
        if (!data_services) {
            return std::unexpected(data_services.error());
        }
        auto implementation = std::make_unique<ModuleManager::Impl>(
            std::move(process_services->channels), std::move(process_services->processes),
            std::move(data_services->output), std::move(data_services->bus));
        implementation->instance = std::move(*instance);
        implementation->locks = std::make_unique<detail::LockRegistry>(implementation->instance);
        implementation->instance_name = std::move(name);
        auto parameters = create_param_services(*implementation->locks, implementation->instance);
        if (!parameters) {
            return std::unexpected(
                wrap_error(ErrorCode::ParamFailed, ErrorOperation::Create, parameters.error()));
        }
        implementation->params = std::move(*parameters);
        auto logs = log::detail::Manager::create(*implementation->channels, *implementation->output,
                                                 implementation->instance_name);
        if (logs) {
            implementation->logs = std::move(*logs);
        } else {
            std::cerr << "Log unavailable: " << error_to_string(logs.error()) << '\n';
        }
        implementation->host_executable = std::move(host_executable);
        auto directory = implementation->reserve_socket_directory();
        if (!directory) {
            return std::unexpected(directory.error());
        }
        // Construct the owner before launching; its destructor can roll back a failed thread start.
        auto manager = std::unique_ptr<ModuleManager>(new ModuleManager(std::move(implementation)));
        manager->impl_->worker = std::jthread([state = manager->impl_.get()](std::stop_token token) {
            state->monitor(token);
        });
        return manager;
    } catch (const std::system_error& error) {
        return std::unexpected(
            Error{ErrorCode::ThreadFailed, ErrorOperation::StartWorker, osal::native_error(error.code())});
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(ErrorCode::ResourceExhausted, ErrorOperation::Create));
    }
}

Result<detail::LockDescriptor> detail::ManagerAccess::create_lock(ModuleManager& manager) {
    {
        std::lock_guard lock(manager.impl_->mutex);
        if (manager.impl_->closing) {
            return std::unexpected(failure(ErrorCode::Closing, ErrorOperation::AccessLock));
        }
    }
    // Admission linearizes above. In-flight creation may finish during shutdown;
    // the registry retains it until destruction, without blocking supervision.
    return manager.impl_->locks->create();
}

Result<void> detail::ManagerAccess::configure_params(ModuleManager& manager, std::filesystem::path data_root) {
    auto result = manager.impl_->params->configure(data_root / manager.impl_->instance_name / "params.toml");
    if (!result) {
        return std::unexpected(wrap_error(ErrorCode::ParamFailed, ErrorOperation::AccessParam, result.error()));
    }
    return {};
}
Result<std::string> detail::ManagerAccess::param_command(ModuleManager& manager, std::vector<std::string> args, std::stop_token cancellation) {
    auto result = manager.impl_->params->command(std::move(args), cancellation);
    if (!result) {
        return std::unexpected(wrap_error(ErrorCode::ParamFailed, ErrorOperation::AccessParam, result.error()));
    }
    return result;
}
Result<void> detail::ManagerAccess::register_param(ModuleManager& manager, std::string name, param::Value defaults) {
    auto result = manager.impl_->params->register_framework(std::move(name), std::move(defaults));
    if (!result) {
        return std::unexpected(wrap_error(ErrorCode::ParamFailed, ErrorOperation::AccessParam, result.error()));
    }
    return {};
}
std::string detail::ManagerAccess::param_diagnostic(ModuleManager& manager, std::uint64_t& cursor) {
    return manager.impl_->params->diagnostic(cursor);
}
Result<param::Snapshot> detail::ManagerAccess::param_snapshot(ModuleManager& manager) {
    auto result = manager.impl_->params->inspect();
    if (!result) {
        return std::unexpected(wrap_error(ErrorCode::ParamFailed, ErrorOperation::AccessParam, result.error()));
    }
    return result;
}

ModuleManager::~ModuleManager() {
    {
        std::lock_guard registry_lock(impl_->mutex);
        impl_->closing = true;
    }

    // Keep supervision running while every Module receives its stop grace period.
    auto records = impl_->snapshot_records();
    auto deadline = Clock::now();
    for (auto& record : records) {
        std::lock_guard record_lock(record->mutex);
        request_user_stop(*record);
        deadline = (std::max)(deadline, Clock::now() + record->module_type.policy.stop_timeout +
                                            shutdown_observation_grace);
    }
    impl_->wake.notify_all();

    for (auto& record : records) {
        std::unique_lock record_lock(record->mutex);
        record->changed.wait_until(record_lock, deadline, [&] {
            return terminal(record->snapshot.state);
        });
    }

    impl_->worker.request_stop();
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    // After the supervisor joins, this thread exclusively performs final cleanup.
    for (auto& record : records) {
        std::lock_guard record_lock(record->mutex);
        if (record->process) {
            (void)record->process->terminate();
        }
        if (record->session) {
            record->session->close();
        }
        close_listener(*record);
    }
    impl_->params.reset();

    // Normal destructor-driven stop also drains observed generations. The
    // bounded fallback does not pretend that unobserved process exits occurred.
    (void)impl_->output->wait_until(Clock::now() + output_drain_timeout);
    impl_->logs.reset();
    impl_->channels.reset();
    std::error_code remove_error;
    std::filesystem::remove(impl_->directory, remove_error);
}

output_hub::OutputHub& ModuleManager::output_hub() noexcept {
    return *impl_->output;
}

// ModuleType registration and Module startup.

Result<void> ModuleManager::register_module_type(ModuleType module_type) {
    if (!valid_policy(module_type.policy)) {
        return std::unexpected(failure(ErrorCode::InvalidPolicy, ErrorOperation::RegisterType));
    }
    if (module_type.name.empty() || module_type.name.size() > module_name_limit_bytes ||
        module_type.name.find('\0') != std::string::npos ||
        (!module_type.python && !module_type.library.is_absolute())) {
        return std::unexpected(failure(ErrorCode::InvalidArgument, ErrorOperation::RegisterType));
    }
    if (module_type.python) {
        const auto& python = *module_type.python;
        if (!module_type.library.empty() || !python.script.is_absolute() || !python.bootstrap.is_absolute() ||
            (!python.executable.empty() && !python.executable.is_absolute())) {
            return std::unexpected(failure(ErrorCode::InvalidArgument, ErrorOperation::RegisterType));
        }
    }

    std::lock_guard registry_lock(impl_->mutex);
    if (impl_->closing) {
        return std::unexpected(failure(ErrorCode::Closing, ErrorOperation::RegisterType));
    }
    if (impl_->reserved_commands.contains(module_type.name)) {
        return std::unexpected(failure(ErrorCode::NameReserved, ErrorOperation::RegisterType));
    }
    if (impl_->module_types.contains(module_type.name) || impl_->programs.contains(module_type.name) ||
        impl_->find_locked(module_type.name)) {
        return std::unexpected(failure(ErrorCode::NameExists, ErrorOperation::RegisterType));
    }
    impl_->module_types.emplace(module_type.name, std::move(module_type));
    return {};
}

Result<MID> ModuleManager::start_module(const std::string& name, std::vector<std::string> args,
                                       ModuleStartOptions options) {
    auto encoded = control::encode_arguments(args);
    if (!encoded) {
        return std::unexpected(
            wrap_error(ErrorCode::InvalidArgument, ErrorOperation::Start, encoded.error()));
    }

    std::lock_guard registry_lock(impl_->mutex);
    if (impl_->closing) {
        return std::unexpected(failure(ErrorCode::Closing, ErrorOperation::Start));
    }
    const auto entry = impl_->module_types.find(name);
    if (entry == impl_->module_types.end()) {
        return std::unexpected(failure(ErrorCode::ModuleTypeNotFound, ErrorOperation::Start));
    }
    if (impl_->records.size() >= module_record_limit) {
        return std::unexpected(failure(ErrorCode::CapacityExceeded, ErrorOperation::Start));
    }
    auto mid = new_mid();
    if (!mid) {
        return std::unexpected(mid.error());
    }

    auto record = std::make_shared<Record>();
    record->module_type = entry->second;
    record->snapshot.mid = *mid;
    record->snapshot.generation = *mid;
    record->snapshot.module_type = name;
    record->snapshot.plugin_path = entry->second.library;
    if (entry->second.python) {
        record->snapshot.python_script = entry->second.python->script;
    }
    record->snapshot.debug = options.debug;
    record->arguments = std::move(args);
    impl_->records.push_back(record);
    impl_->wake.notify_all();
    return *mid;
}

Result<void> ModuleManager::wait_debug_ready(MID mid, Deadline deadline, std::stop_token stop) {
    auto record = impl_->find(mid);
    if (!record) {
        return std::unexpected(failure(ErrorCode::ModuleNotFound, ErrorOperation::WaitDebugReady));
    }

    std::unique_lock lock(record->mutex);
    if (!record->snapshot.debug) {
        return std::unexpected(failure(ErrorCode::InvalidArgument, ErrorOperation::WaitDebugReady));
    }

    while (!record->snapshot.debug_waiting && !record->fault && !record->user_stop &&
           !terminal(record->snapshot.state) && !record->debug_gate_seen &&
           !stop.stop_requested() && Clock::now() < deadline) {
        record->changed.wait_until(lock, (std::min)(deadline, Clock::now() + supervisor_poll_interval));
    }

    if (stop.stop_requested()) {
        return std::unexpected(failure(ErrorCode::Cancelled, ErrorOperation::WaitDebugReady));
    }

    if (record->fault && record->snapshot.last_failure) {
        return std::unexpected(record->snapshot.last_failure->error);
    }

    if (record->user_stop || terminal(record->snapshot.state)) {
        return std::unexpected(failure(ErrorCode::NotRunning, ErrorOperation::WaitDebugReady));
    }

    if (record->snapshot.debug_waiting) {
        return {};
    }

    return std::unexpected(failure(record->debug_gate_seen ? ErrorCode::InvalidArgument : ErrorCode::Timeout,
                                    ErrorOperation::WaitDebugReady));
}

Result<void> ModuleManager::continue_debug(MID mid) {
    auto record = impl_->find(mid);
    if (!record) {
        return std::unexpected(failure(ErrorCode::ModuleNotFound, ErrorOperation::ContinueDebug));
    }

    std::lock_guard lock(record->mutex);
    if (!record->snapshot.debug || !record->snapshot.debug_waiting) {
        return std::unexpected(failure(ErrorCode::InvalidArgument, ErrorOperation::ContinueDebug));
    }

    if (record->user_stop || record->fault || record->snapshot.state != ModuleState::Starting) {
        return std::unexpected(failure(ErrorCode::NotRunning, ErrorOperation::ContinueDebug));
    }

    auto sent = record->session->send({control::Kind::Event, detail::lifecycle, detail::debug_continue});
    if (!sent) {
        return std::unexpected(wrap_error(ErrorCode::ChannelFailed, ErrorOperation::ContinueDebug, sent.error()));
    }

    record->snapshot.debug_waiting = false;
    record->changed.notify_all();
    return {};
}

Result<void> ModuleManager::wait_running(const ModuleKey& key, Deadline deadline, std::stop_token stop) {
    auto record = impl_->find(key);
    if (!record) {
        return std::unexpected(failure(ErrorCode::ModuleNotFound, ErrorOperation::WaitRunning));
    }
    std::unique_lock record_lock(record->mutex);
    while (!record->user_stop && record->snapshot.state != ModuleState::Running &&
           !terminal(record->snapshot.state) && !record->startup_rejected && matches(*record, key) &&
           !stop.stop_requested() && Clock::now() < deadline) {
        record->changed.wait_until(record_lock,
                                   (std::min)(deadline, Clock::now() + supervisor_poll_interval));
    }
    if (!matches(*record, key)) {
        return std::unexpected(failure(ErrorCode::GenerationChanged, ErrorOperation::WaitRunning));
    }
    if (stop.stop_requested()) {
        return std::unexpected(failure(ErrorCode::Cancelled, ErrorOperation::WaitRunning));
    }
    if (record->snapshot.state == ModuleState::Running && !record->user_stop) {
        return {};
    }
    if (record->snapshot.last_failure && record->snapshot.last_failure->mid == record->snapshot.mid) {
        return std::unexpected(record->snapshot.last_failure->error);
    }
    if (record->user_stop || terminal(record->snapshot.state)) {
        return std::unexpected(failure(ErrorCode::NotRunning, ErrorOperation::WaitRunning));
    }
    return std::unexpected(failure(ErrorCode::Timeout, ErrorOperation::WaitRunning));
}

Result<void> ModuleManager::wait_started(const ModuleKey& key, std::stop_token stop) {
    auto record = impl_->find(key);
    if (!record) {
        return std::unexpected(failure(ErrorCode::ModuleNotFound, ErrorOperation::WaitRunning));
    }
    auto started = wait_running(key, Clock::now() + record->module_type.policy.startup_timeout + readiness_wait_grace,
                                stop);
    if (!started && started.error().is(ErrorCode::NotRunning)) {
        std::lock_guard lock(record->mutex);
        // A short Manual main may finish between Ready and the waiter's next turn.
        // A confirmed completion proves startup; an arbitrary exit code does not.
        if (matches(*record, key) && record->normal_completion) {
            return {};
        }
    }
    return started;
}

Result<void> ModuleManager::reserve_command_name(const std::string& name) {
    std::lock_guard registry_lock(impl_->mutex);
    if (name.empty()) {
        return std::unexpected(failure(ErrorCode::InvalidName, ErrorOperation::ReserveName));
    }
    if (impl_->closing) {
        return std::unexpected(failure(ErrorCode::Closing, ErrorOperation::ReserveName));
    }
    if (impl_->module_types.contains(name) || impl_->programs.contains(name) || impl_->find_locked(name)) {
        return std::unexpected(failure(ErrorCode::NameExists, ErrorOperation::ReserveName));
    }
    impl_->reserved_commands.insert(name);
    return {};
}

void ModuleManager::begin_shutdown() {
    // Hold admission lock until every existing record has been marked: no start can slip through.
    std::lock_guard registry_lock(impl_->mutex);
    impl_->closing = true;
    for (auto& record : impl_->records) {
        std::lock_guard record_lock(record->mutex);
        request_user_stop(*record);
        record->changed.notify_all();
    }
    impl_->wake.notify_all();
}

std::vector<ModuleSnapshot> ModuleManager::snapshots() const {
    std::vector<ModuleSnapshot> result;
    for (auto& record : impl_->snapshot_records()) {
        std::lock_guard record_lock(record->mutex);
        result.push_back(record->snapshot);
    }
    return result;
}

// Finish means both OS exit observation and BusManager cleanup have completed.
// A caller deadline only ends this wait; the supervisor continues stopping children.
Result<void> ModuleManager::wait_shutdown(vinc::Deadline deadline) {
    begin_shutdown();
    for (auto& record : impl_->snapshot_records()) {
        std::unique_lock record_lock(record->mutex);
        if (!record->changed.wait_until(record_lock, deadline, [&] {
                return terminal(record->snapshot.state) && !record->process;
            })) {
            return std::unexpected(failure(ErrorCode::Timeout, ErrorOperation::WaitShutdown));
        }
    }

    auto bus = impl_->bus_manager->drain_until(deadline);
    if (!bus) {
        return std::unexpected(wrap_error(ErrorCode::BusFailed, ErrorOperation::WaitShutdown, bus.error()));
    }
    auto output = impl_->output->wait_until(deadline);
    if (!output) {
        return std::unexpected(
            wrap_error(ErrorCode::OutputFailed, ErrorOperation::WaitShutdown, output.error()));
    }

    return {};
}

Result<void> ModuleManager::stop_module(const ModuleKey& key, vinc::Deadline deadline) {
    auto record = impl_->find(key);
    if (!record) {
        return std::unexpected(failure(ErrorCode::ModuleNotFound, ErrorOperation::Stop));
    }

    std::unique_lock record_lock(record->mutex);
    if (!matches(*record, key)) {
        return std::unexpected(failure(ErrorCode::GenerationChanged, ErrorOperation::Stop));
    }

    request_user_stop(*record);
    impl_->wake.notify_all();

    if (!record->changed.wait_until(record_lock, deadline, [&] {
            return terminal(record->snapshot.state);
        })) {
        return std::unexpected(failure(ErrorCode::Timeout, ErrorOperation::Stop));
    }
    return {};
}

Result<void> ModuleManager::forget_module(const ModuleKey& key) {
    std::lock_guard registry_lock(impl_->mutex);
    auto record = impl_->find_locked(key);
    if (!record) {
        return std::unexpected(failure(ErrorCode::ModuleNotFound, ErrorOperation::Forget));
    }

    std::lock_guard record_lock(record->mutex);
    if (!matches(*record, key)) {
        return std::unexpected(failure(ErrorCode::GenerationChanged, ErrorOperation::Forget));
    }

    if (!terminal(record->snapshot.state) || record->process) {
        return std::unexpected(failure(ErrorCode::Busy, ErrorOperation::Forget));
    }

    std::erase(impl_->records, record);
    return {};
}

std::optional<ModuleSnapshot> ModuleManager::inspect(const ModuleKey& key) const {
    auto record = impl_->find(key);
    if (!record) {
        return {};
    }

    std::lock_guard record_lock(record->mutex);
    if (!matches(*record, key)) {
        return {};
    }

    return record->snapshot;
}

// Commands and completion requests execute inside the Module process.

Result<int32_t> ModuleManager::command(const ModuleKey& key, std::vector<std::string> args, Deadline deadline,
                                       std::stop_token stop) {
    auto response = impl_->request(key, detail::execute, std::move(args), deadline, stop);
    if (!response) {
        return std::unexpected(response.error());
    }
    auto body = detail::decode_response(response->payload);
    if (!body) {
        return std::unexpected(body.error());
    }
    control::Reader reader(*body);
    const auto value = reader.u32();
    if (!reader.done()) {
        return std::unexpected(failure(ErrorCode::Protocol, ErrorOperation::Command));
    }
    return std::bit_cast<int32_t>(value);
}

Result<std::vector<std::string>> ModuleManager::complete(const ModuleKey& key, std::vector<std::string> args,
                                                         Deadline deadline) {
    auto response = impl_->request(key, detail::complete, std::move(args), deadline);
    if (!response) {
        return std::unexpected(response.error());
    }
    auto body = detail::decode_response(response->payload);
    if (!body) {
        return std::unexpected(body.error());
    }
    auto decoded = control::decode_arguments(*body);
    if (!decoded) {
        return std::unexpected(wrap_error(ErrorCode::Protocol, ErrorOperation::Complete, decoded.error()));
    }
    return std::move(*decoded);
}

Result<void> ModuleManager::unregister_module_type(const std::string& name) {
    std::lock_guard registry_lock(impl_->mutex);
    if (!impl_->module_types.erase(name)) {
        return std::unexpected(failure(ErrorCode::ModuleTypeNotFound, ErrorOperation::UnregisterType));
    }
    return {};
}

Result<int32_t> ModuleManager::call_module_type_interface(const std::string& name,
                                                          std::vector<std::string> args) {
    auto started = start_module(name, std::move(args));
    if (!started) {
        return std::unexpected(started.error());
    }
    auto running = wait_started(*started);
    if (!running) {
        return std::unexpected(running.error());
    }
    return 0;
}

Result<int32_t> ModuleManager::call_module_interface(const std::string& name, std::vector<std::string> args) {
    return command(name, std::move(args), Clock::now() + adapter_command_timeout);
}

Result<std::vector<std::string>> ModuleManager::call_module_type_autoComplete(const std::string& name,
                                                                              std::vector<std::string>) {
    if (!has_module_type(name)) {
        return std::unexpected(failure(ErrorCode::ModuleTypeNotFound, ErrorOperation::Complete));
    }
    return {};
}

Result<std::vector<std::string>> ModuleManager::call_module_autoComplete(const std::string& name,
                                                                         std::vector<std::string> args) {
    return complete(name, std::move(args), Clock::now() + adapter_completion_timeout);
}

Result<void> ModuleManager::register_micro_program(const std::string& name, CommandFunction main,
                                                   AutoCompleteFunction completion) {
    std::lock_guard registry_lock(impl_->mutex);
    if (name.empty() || !main) {
        return std::unexpected(failure(ErrorCode::InvalidArgument, ErrorOperation::RegisterMicroProgram));
    }
    if (impl_->closing) {
        return std::unexpected(failure(ErrorCode::Closing, ErrorOperation::RegisterMicroProgram));
    }
    if (impl_->reserved_commands.contains(name)) {
        return std::unexpected(failure(ErrorCode::NameReserved, ErrorOperation::RegisterMicroProgram));
    }
    if (impl_->programs.contains(name) || impl_->module_types.contains(name) || impl_->find_locked(name)) {
        return std::unexpected(failure(ErrorCode::NameExists, ErrorOperation::RegisterMicroProgram));
    }
    impl_->programs.emplace(name, microProgram{name, std::move(main), std::move(completion)});
    return {};
}

Result<int32_t> ModuleManager::call_micro_program(const std::string& name, std::vector<std::string> args) {
    CommandFunction function;
    {
        std::lock_guard registry_lock(impl_->mutex);
        const auto program = impl_->programs.find(name);
        if (program == impl_->programs.end()) {
            return std::unexpected(failure(ErrorCode::MicroProgramNotFound, ErrorOperation::MicroProgram));
        }
        function = program->second.main_func;
    }
    try {
        auto result = function(args);
        if (!result) {
            return std::unexpected(
                wrap_error(ErrorCode::HookFailed, ErrorOperation::MicroProgram, result.error()));
        }
        return *result;
    } catch (...) {
        return std::unexpected(failure(ErrorCode::HookException, ErrorOperation::MicroProgram));
    }
}

Result<std::vector<std::string>>
ModuleManager::call_micro_program_autoComplete(const std::string& name, std::vector<std::string> args) {
    AutoCompleteFunction function;
    {
        std::lock_guard registry_lock(impl_->mutex);
        const auto program = impl_->programs.find(name);
        if (program == impl_->programs.end()) {
            return std::unexpected(
                failure(ErrorCode::MicroProgramNotFound, ErrorOperation::MicroProgramComplete));
        }
        function = program->second.autoComplete_func;
    }
    if (!function) {
        return {};
    }
    try {
        auto result = function(args);
        if (!result) {
            return std::unexpected(
                wrap_error(ErrorCode::HookFailed, ErrorOperation::MicroProgramComplete, result.error()));
        }
        return std::move(*result);
    } catch (...) {
        return std::unexpected(failure(ErrorCode::HookException, ErrorOperation::MicroProgramComplete));
    }
}

// Registry queries return copies, so callers do not retain internal locks or references.

std::vector<std::string> ModuleManager::get_registered_module_type_names() const {
    std::lock_guard registry_lock(impl_->mutex);
    std::vector<std::string> names;
    for (const auto& [name, _] : impl_->module_types) {
        names.push_back(name);
    }
    return names;
}

std::vector<std::string> ModuleManager::get_active_module_names() const {
    std::vector<std::string> names;
    for (auto& record : impl_->snapshot_records()) {
        std::lock_guard record_lock(record->mutex);
        if (!record->snapshot.name.empty() && !terminal(record->snapshot.state)) {
            names.push_back(record->snapshot.name);
        }
    }
    return names;
}

std::vector<std::string> ModuleManager::get_micro_program_names() const {
    std::lock_guard registry_lock(impl_->mutex);
    std::vector<std::string> names;
    for (const auto& [name, _] : impl_->programs) {
        names.push_back(name);
    }
    return names;
}

bool ModuleManager::has_module_type(const std::string& name) const {
    std::lock_guard registry_lock(impl_->mutex);
    return impl_->module_types.contains(name);
}

bool ModuleManager::has_active_module(const std::string& name) const {
    const auto snapshot = inspect(name);
    return snapshot && !terminal(snapshot->state);
}

bool ModuleManager::has_micro_program(const std::string& name) const {
    std::lock_guard registry_lock(impl_->mutex);
    return impl_->programs.contains(name);
}
} // namespace vinc

namespace vinc::detail {
Result<void> ManagerAccess::configure_log(ModuleManager& manager, std::filesystem::path root,
                                          log::FileOptions options) {
    if (!manager.impl_->logs) {
        return std::unexpected(Error{module_system::ErrorCode::LogFailed, module_system::ErrorOperation::AccessLog});
    }
    auto result = manager.impl_->logs->configure(root / manager.impl_->instance_name / "logs", options);
    if (!result) {
        return std::unexpected(wrap_error(module_system::ErrorCode::LogFailed, module_system::ErrorOperation::AccessLog, result.error()));
    }
    return {};
}

Result<std::string> ManagerAccess::log_command(ModuleManager& manager, const std::vector<std::string>& args) {
    if (!manager.impl_->logs) {
        return std::unexpected(Error{module_system::ErrorCode::LogFailed, module_system::ErrorOperation::AccessLog});
    }
    auto result = manager.impl_->logs->command(args);
    if (!result) {
        return std::unexpected(wrap_error(module_system::ErrorCode::LogFailed, module_system::ErrorOperation::AccessLog, result.error()));
    }
    return result;
}

std::string ManagerAccess::log_take(ModuleManager& manager, log::View& view) {
    if (!manager.impl_->logs) {
        return {};
    }
    return manager.impl_->logs->take(view);
}

std::string ManagerAccess::log_notification(ModuleManager& manager, std::uint64_t& cursor) {
    if (!manager.impl_->logs) {
        return {};
    }
    return manager.impl_->logs->notification(cursor);
}
}

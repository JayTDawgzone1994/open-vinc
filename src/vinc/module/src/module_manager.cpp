// Central module supervisor: allocate MIDs, approve names, launch through OSAL, wait for Ready,
// route requests, and stop/reap each generation before retiring its Bus resources.

#include <module/module_manager.hpp>
#include "module_protocol.hpp"
#include "runtime.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <limits>

namespace vinc {
namespace {
using namespace std::chrono_literals;
using control::Clock;
using control::Deadline;
control::Error failure(control::ErrorCode code, std::string message) { return {code, std::move(message), {}}; }
bool terminal(ModuleState s) { return s == ModuleState::Stopped || s == ModuleState::Failed; }
std::string utf8(const std::filesystem::path& path) {
    const auto s = path.u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()};
}
std::string nonce() {
    std::random_device random;
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (int i = 0; i < 4; ++i) { const auto n = random(); for (int bit = 28; bit >= 0; bit -= 4) result += hex[(n >> bit) & 15]; }
    return result;
}
std::atomic<MID> next_mid{1};
MID new_mid() {
    auto mid = next_mid.load(std::memory_order_relaxed);
    do {
        if (mid == UINT64_MAX) throw std::runtime_error("module MIDs exhausted");
    } while (!next_mid.compare_exchange_weak(mid, mid + 1, std::memory_order_relaxed));
    return mid;
}
// One logical Module across restarts. Record::mutex protects its current generation.
// Keep pending_request after a caller timeout: the child may still be executing it.
struct Record {
    std::mutex mutex;
    std::condition_variable changed;
    ModuleType module_type;
    ModuleSnapshot snapshot;
    std::vector<std::string> arguments;
    std::optional<osal::Process> process;
    std::uint64_t output_capture = 0;
    std::optional<osal::Socket> listener;
    std::filesystem::path socket_path;
    std::shared_ptr<control::Session> session;
    std::string token;
    Deadline startup_deadline{}, stop_deadline{}, restart_at{}, next_terminate{};
    bool user_stop = false, fault = false, stop_sent = false;
    bool name_confirmed = false, startup_rejected = false;
    std::optional<control::Frame> name_request;
    bool pending_waiter = false;
    std::uint64_t next_request = 1, pending_request = 0;
    std::uint16_t pending_operation = 0;
    std::optional<control::Frame> response;
};
bool matches(const Record& record, const ModuleKey& key) {
    if (const auto* mid = std::get_if<MID>(&key)) return *mid && record.snapshot.mid == *mid;
    const auto& name = std::get<std::string>(key);
    return !name.empty() && record.snapshot.name == name;
}
void close_listener(Record& record) {
    record.listener.reset();
    if (!record.socket_path.empty()) {
        std::error_code ec; std::filesystem::remove(record.socket_path, ec);
        if (!ec) record.socket_path.clear();
    }
}
void record_failure(Record& record, std::string message, std::optional<osal::OsError> os = {}) {
    record.snapshot.last_error = std::move(message); record.snapshot.os_error = os;
    record.fault = true; record.snapshot.state = ModuleState::Stopping;
    record.stop_deadline = Clock::now(); record.changed.notify_all();
}
}
struct ModuleManager::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::map<std::string, ModuleType> module_types;
    std::vector<std::shared_ptr<Record>> records;
    std::map<std::string, microProgram> programs;
    std::set<std::string> reserved_commands;
    std::unique_ptr<control::Service> channels;
    osal::ProcessService processes;
    // One local Shell plus eight connected remote Shell subscribers.
    output::OutputHub output{output::Limits{.max_subscribers = 9}};
    bus::detail::Broker broker;
    std::filesystem::path directory;
    std::jthread worker;
    bool closing = false;
    Impl(std::unique_ptr<control::Service> service, osal::ProcessService ps, bus::Limits limits)
        : channels(std::move(service)), processes(std::move(ps)), broker(limits) {
        for (int i = 0; i < 8; ++i) {
            directory = std::filesystem::temp_directory_path() / ("ov-" + nonce().substr(0, 12));
            if (std::filesystem::create_directory(directory)) break;
            directory.clear();
        }
        if (directory.empty()) throw std::runtime_error("cannot reserve module socket directory");
#ifndef _WIN32
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
#endif
        worker = std::jthread([this](std::stop_token token) { monitor(token); });
    }
    // Registry lock precedes record locks. The monitor releases a record before
    // entering name registration, so no record -> registry lock cycle exists.
    std::shared_ptr<Record> find_locked(const ModuleKey& key) const {
        for (auto& record : records) { std::lock_guard lock(record->mutex); if (matches(*record, key)) return record; }
        return {};
    }
    std::shared_ptr<Record> find(const ModuleKey& key) const {
        std::lock_guard lock(mutex); return find_locked(key);
    }
    std::vector<std::shared_ptr<Record>> snapshot_records() const {
        std::lock_guard lock(mutex); return records;
    }
    // Every launch gets a fresh generation, token and endpoint. Replies and Bus ownership
    // from an old process must never be mistaken for the newly started process.
    void launch(Record& record) {
        // The first attempt already owns its MID before spawn; every retry gets another.
        if (record.snapshot.restarts) record.snapshot.mid = new_mid();
        record.snapshot.generation = record.snapshot.mid; // Existing wire/Bus generation uses this same identity.
        record.name_confirmed = false; record.name_request.reset();
        record.snapshot.pid = 0; record.fault = false; record.stop_sent = false; record.next_terminate = {};
        record.pending_request = 0; record.response.reset(); record.token = nonce();
        record.socket_path = directory / (std::to_string(record.snapshot.generation) + ".sock");
        auto listener = osal::Socket::open(osal::AddressFamily::Local, osal::SocketType::Stream);
        if (!listener) { record_failure(record, "open module listener", listener.error()); return; }
        record.listener.emplace(std::move(*listener));
        auto bound = record.listener->bind(osal::LocalEndpoint{utf8(record.socket_path)});
        if (!bound) { record_failure(record, "bind module listener", bound.error()); return; }
        auto listening = record.listener->listen(4);
        if (!listening) { record_failure(record, "listen for module", listening.error()); return; }
        const auto& policy = record.module_type.policy;
        osal::SpawnOptions options;
        options.new_process_group = true; // central coordinates stop; terminal Ctrl+C must not kill children first
        options.executable = record.module_type.executable;
        options.arguments = {"--vinc-module-v2", utf8(record.socket_path),
            record.snapshot.name.empty() ? record.module_type.module_base_name : record.snapshot.name, record.token,
            std::to_string(record.snapshot.generation), std::to_string(policy.heartbeat_interval.count()),
            std::to_string(policy.heartbeat_timeout.count()), std::to_string(policy.startup_timeout.count()),
            std::to_string(policy.stop_timeout.count()), record.module_type.name};
        options.arguments.insert(options.arguments.end(), record.arguments.begin(), record.arguments.end());
        record.startup_deadline = Clock::now() + policy.startup_timeout;
        auto child = output.spawn(processes, options, record.snapshot.name.empty() ? "starting:" + std::to_string(record.snapshot.mid) : record.snapshot.name, record.snapshot.generation);
        if (!child) { record_failure(record, "spawn module", child.error()); return; }
        record.output_capture = child->capture;
        record.process.emplace(std::move(child->process)); record.snapshot.pid = record.process->pid();
        record.snapshot.state = ModuleState::Starting; record.changed.notify_all();
    }
    // Release the completed run and apply restart policy to the same logical Module.
    // An explicit user stop suppresses restart even if retries remain.
    void finished(Record& record) {
        if (record.session) record.session->close(); record.session.reset(); close_listener(record);
        if (record.process) { (void)processes.forget(record.process->id()); record.process.reset(); }
        record.pending_request = 0; record.response.reset();
        if (!record.user_stop && !record.startup_rejected && record.snapshot.restarts < record.module_type.policy.max_restarts) {
            ++record.snapshot.restarts; record.restart_at = Clock::now() + record.module_type.policy.restart_delay;
            record.snapshot.state = ModuleState::RestartWaiting;
        } else record.snapshot.state = record.user_stop ? ModuleState::Stopped : ModuleState::Failed;
        record.changed.notify_all();
    }
    void tick(Record& record) {
        const auto now = Clock::now();
        if (terminal(record.snapshot.state)) return;
        if (record.user_stop && !record.process) { finished(record); return; }
        if (record.snapshot.state == ModuleState::Queued ||
            (record.snapshot.state == ModuleState::RestartWaiting && now >= record.restart_at)) launch(record);
        if (record.snapshot.state == ModuleState::RestartWaiting) return;
        if (record.process) {
            auto exit = record.process->try_wait();
            if (!exit) { record_failure(record, "observe module", exit.error()); }
            else if (*exit) {
                record.snapshot.last_exit = **exit;
                output.exited(record.output_capture);
                // Only confirmed OS exit permits reclaiming this generation's shared reader leases.
                // A missing heartbeat alone does not prove that the process has stopped touching memory.
                broker.exited(record.snapshot.generation);
                if (!record.user_stop && !record.fault) record.snapshot.last_error = "module exited unexpectedly";
                finished(record); return;
            }
        }
        if (record.fault && !record.process) { finished(record); return; }
        if (record.user_stop || record.fault) {
            if (record.snapshot.state != ModuleState::Stopping) {
                record.snapshot.state = ModuleState::Stopping;
                record.stop_deadline = now + record.module_type.policy.stop_timeout;
                record.changed.notify_all();
            }
            if (!record.stop_sent && record.session && record.session->state() == control::State::Open) {
                auto sent = record.session->send({control::Kind::Event, detail::lifecycle, detail::stop});
                record.stop_sent = bool(sent);
            }
            if (now >= record.stop_deadline && now >= record.next_terminate && record.process) {
                auto killed = record.process->terminate(); record.next_terminate = now + 1s;
                if (!killed) { record.snapshot.last_error = "terminate module"; record.snapshot.os_error = killed.error(); }
            }
            return;
        }
        if (record.listener && !record.session) {
            auto accepted = record.listener->try_accept();
            if (accepted) {
                control::Options options;
                options.generation = record.snapshot.generation; options.token = record.token;
                options.heartbeat_interval = record.module_type.policy.heartbeat_interval;
                options.heartbeat_timeout = record.module_type.policy.heartbeat_timeout;
                options.handshake_timeout = record.module_type.policy.startup_timeout;
                auto attached = channels->attach(std::move(*accepted), options);
                if (!attached) { record_failure(record, attached.error().message, attached.error().os_error); return; }
                record.session = *attached; close_listener(record);
            } else if (accepted.error().code != osal::ErrorCode::WouldBlock) {
                record_failure(record, "accept module connection", accepted.error()); return;
            }
        }
        if (record.session) {
            for (int budget = 0; budget < 16; ++budget) {
                auto received = record.session->try_receive();
                if (!received) { record_failure(record, received.error().message, received.error().os_error); break; }
                if (!*received) break;
                auto frame = std::move(**received);
                if (frame.domain == bus::detail::bus_domain) {
                    if (!record.name_confirmed) { record_failure(record, "Bus request before name registration"); break; }
                    if (!broker.submit(record.snapshot.generation, record.snapshot.name, record.session, std::move(frame))) {
                        record_failure(record, "Bus control queue exhausted"); break;
                    }
                } else if (frame.kind == control::Kind::Request && frame.domain == detail::lifecycle && frame.operation == detail::register_name) {
                    if (record.name_confirmed || record.name_request || frame.request != 1 || record.snapshot.state != ModuleState::Starting) {
                        record_failure(record, "invalid name registration request"); break;
                    }
                    record.name_request = std::move(frame);
                } else if (frame.kind == control::Kind::Event && frame.domain == detail::lifecycle && frame.operation == detail::ready) {
                    control::Reader reader(frame.payload); const auto name = reader.string();
                    if (!record.name_confirmed || name != record.snapshot.name || !reader.done() || record.snapshot.state != ModuleState::Starting) {
                        record_failure(record, "invalid ready message"); break;
                    }
                    record.snapshot.state = ModuleState::Running; record.changed.notify_all();
                } else if (frame.kind == control::Kind::Response && frame.domain == detail::commands) {
                    if (record.pending_request == frame.request && record.pending_operation == frame.operation) {
                        if (record.pending_waiter) record.response = std::move(frame);
                        else record.pending_request = 0;
                        record.changed.notify_all();
                    } // A timed-out request stays busy until its reply or process exit.
                } else { record_failure(record, "unexpected module message"); break; }
            }
        }
        if (record.snapshot.state == ModuleState::Starting && now >= record.startup_deadline)
            record_failure(record, "module readiness timeout");
    }
    void register_name(const std::shared_ptr<Record>& record) {
        std::lock_guard registry_lock(mutex);
        std::lock_guard lock(record->mutex);
        if (!record->name_request || record->fault || record->user_stop || closing) return;
        auto request = std::move(*record->name_request); record->name_request.reset();
        control::Reader reader(request.payload); auto name = reader.string();
        std::string error;
        if (!reader.done() || name.empty() || name.size() > 128 ||
            std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) error = "invalid module name";
        else if (!record->snapshot.name.empty() && name != record->snapshot.name) error = "restart must retain its registered module name: " + record->snapshot.name;
        else if (reserved_commands.contains(name) || module_types.contains(name) || programs.contains(name)) error = "module name conflicts with a command: " + name;
        std::shared_ptr<Record> previous;
        if (error.empty()) for (auto& other : records) {
            if (other == record) continue;
            std::lock_guard other_lock(other->mutex);
            if (other->snapshot.name != name) continue;
            if (terminal(other->snapshot.state) && !other->process) previous = other;
            else error = "module already exists: " + name;
            break;
        }
        control::Frame response{control::Kind::Response, detail::lifecycle, detail::register_name, request.request};
        control::put_u32(response.payload, error.empty() ? 0 : 1);
        control::put_string(response.payload, error.empty() ? name : error);
        if (error.empty()) {
            // Publish the name while admission remains locked, before acknowledging it.
            output.set_name(record->output_capture, name);
            if (previous) std::erase(records, previous);
            record->snapshot.name = std::move(name); record->name_confirmed = true;
        } else {
            record->snapshot.last_error = error; record->startup_rejected = true;
            record->fault = true; record->snapshot.state = ModuleState::Stopping;
            record->stop_deadline = Clock::now() + record->module_type.policy.stop_timeout;
        }
        auto sent = record->session->send(std::move(response));
        if (!sent) record_failure(*record, sent.error().message, sent.error().os_error);
        record->changed.notify_all();
    }
    void monitor(std::stop_token token) {
        while (!token.stop_requested()) {
            for (auto& record : snapshot_records()) {
                try {
                    { std::lock_guard lock(record->mutex); tick(*record); }
                    register_name(record);
                } catch (const std::exception& e) { std::lock_guard lock(record->mutex); record_failure(*record, e.what()); }
                catch (...) { std::lock_guard lock(record->mutex); record_failure(*record, "module supervisor exception"); }
            }
            std::unique_lock lock(mutex); wake.wait_for(lock, 20ms);
        }
    }
    // Allow one outstanding command/completion per Module. Capture its generation before
    // waiting, and never replay the request if timeout, cancellation or restart loses the result.
    control::Result<control::Frame> request(const ModuleKey& name, std::uint16_t operation,
        std::vector<std::string> args, Deadline deadline, std::stop_token stop = {}) {
        if (stop.stop_requested()) return std::unexpected(failure(control::ErrorCode::Closed, "command wait canceled"));
        auto record = find(name);
        if (!record) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module"));
        control::Bytes payload;
        try { payload = control::encode_arguments(args); }
        catch (const std::length_error& e) { return std::unexpected(failure(control::ErrorCode::InvalidArgument, e.what())); }
        std::unique_lock lock(record->mutex);
        if (!matches(*record, name) || record->snapshot.state != ModuleState::Running || record->user_stop)
            return std::unexpected(failure(control::ErrorCode::Closed, "module is not running"));
        if (record->pending_request) return std::unexpected(failure(control::ErrorCode::Backpressure, "module already has an outstanding request"));
        if (Clock::now() >= deadline) return std::unexpected(failure(control::ErrorCode::Timeout, "request deadline expired"));
        if (!record->next_request) return std::unexpected(failure(control::ErrorCode::Protocol, "request IDs exhausted"));
        const auto id = record->next_request++; const auto generation = record->snapshot.generation;
        record->pending_request = id; record->pending_operation = operation; record->response.reset(); record->pending_waiter = true;
        auto sent = record->session->send({control::Kind::Request, detail::commands, operation, id, std::move(payload)});
        if (!sent) { record->pending_request = 0; return std::unexpected(sent.error()); }
        while (!record->response && !record->user_stop && record->snapshot.state == ModuleState::Running &&
            record->snapshot.generation == generation && !stop.stop_requested() && Clock::now() < deadline)
            record->changed.wait_until(lock, (std::min)(deadline, Clock::now() + 20ms));
        if (record->snapshot.generation != generation)
            return std::unexpected(failure(control::ErrorCode::Closed, "module generation changed; command outcome may be unknown"));
        // Detach the caller without freeing the in-flight request ID. The supervisor clears
        // Busy on a late response or process exit, preventing overlapping commands after cancellation.
        record->pending_waiter = false;
        if (record->user_stop || record->snapshot.state != ModuleState::Running)
            return std::unexpected(failure(control::ErrorCode::Closed, "module session ended; command outcome may be unknown"));
        if (!record->response && stop.stop_requested())
            return std::unexpected(failure(control::ErrorCode::Closed, "command wait canceled; command outcome may be unknown"));
        if (!record->response) return std::unexpected(failure(control::ErrorCode::Timeout, "command outcome may be unknown"));
        record->pending_request = 0;
        auto response = std::move(*record->response); record->response.reset(); return response;
    }
};
ModuleManager::ModuleManager(bus::Limits limits) {
    auto channels = control::Service::create(); auto processes = osal::ProcessService::create();
    if (!channels || !processes) throw std::runtime_error("cannot initialize module services");
    impl_ = std::make_unique<Impl>(std::move(*channels), std::move(*processes), limits);
}
ModuleManager::~ModuleManager() {
    { std::lock_guard lock(impl_->mutex); impl_->closing = true; }
    auto records = impl_->snapshot_records(); auto deadline = Clock::now();
    for (auto& record : records) {
        std::lock_guard lock(record->mutex); record->user_stop = true;
        deadline = (std::max)(deadline, Clock::now() + record->module_type.policy.stop_timeout + 2s);
    }
    impl_->wake.notify_all();
    for (auto& record : records) {
        std::unique_lock lock(record->mutex); record->changed.wait_until(lock, deadline, [&] { return terminal(record->snapshot.state); });
    }
    impl_->worker.request_stop(); impl_->wake.notify_all(); impl_->worker.join();
    for (auto& record : records) {
        std::lock_guard lock(record->mutex);
        if (record->process) (void)record->process->terminate();
        if (record->session) record->session->close(); close_listener(*record);
    }
    impl_->channels.reset();
    // Normal destructor-driven stop also drains observed generations. The
    // bounded fallback does not pretend that unobserved process exits occurred.
    (void)impl_->output.wait_until(Clock::now() + 2s);
    std::error_code ec; std::filesystem::remove(impl_->directory, ec);
}
ModuleManager& ModuleManager::get() { static ModuleManager manager; return manager; }
output::OutputHub& ModuleManager::output_hub() noexcept { return impl_->output; }
control::Result<void> ModuleManager::register_module_type(ModuleType module_type) {
    const auto& p = module_type.policy;
    if (module_type.name.empty() || module_type.name.size() > 128 || module_type.name.find('\0') != std::string::npos ||
        module_type.module_base_name.size() > 128 || module_type.module_base_name.find('\0') != std::string::npos ||
        !module_type.executable.is_absolute() || p.heartbeat_interval <= 0ms || p.heartbeat_timeout <= p.heartbeat_interval ||
        p.heartbeat_timeout > 24h || p.startup_timeout <= 0ms || p.startup_timeout > 24h ||
        p.stop_timeout <= 0ms || p.stop_timeout > 60s || p.restart_delay < 100ms || p.max_restarts > 1000)
        return std::unexpected(failure(control::ErrorCode::InvalidArgument, "invalid module type or policy"));
    if (module_type.module_base_name.empty()) module_type.module_base_name = module_type.name;
    std::lock_guard lock(impl_->mutex);
    if (impl_->reserved_commands.contains(module_type.name) || impl_->reserved_commands.contains(module_type.module_base_name))
        return std::unexpected(failure(control::ErrorCode::InvalidArgument, "module type name or module base name is a reserved command"));
    if (impl_->closing || impl_->reserved_commands.contains(module_type.name) || impl_->module_types.contains(module_type.name) || impl_->programs.contains(module_type.name) || impl_->find_locked(module_type.name))
        return std::unexpected(failure(control::ErrorCode::InvalidArgument, "module type name already registered or manager closing"));
    impl_->module_types.emplace(module_type.name, std::move(module_type)); return {};
}
control::Result<MID> ModuleManager::start_module(const std::string& module_type_name, std::vector<std::string> args) {
    try { (void)control::encode_arguments(args); }
    catch (const std::length_error& e) { return std::unexpected(failure(control::ErrorCode::InvalidArgument, e.what())); }
    std::lock_guard lock(impl_->mutex); auto module_type = impl_->module_types.find(module_type_name);
    if (impl_->closing || module_type == impl_->module_types.end()) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module type or closing manager"));
    if (impl_->records.size() >= 256) return std::unexpected(failure(control::ErrorCode::Backpressure, "module record limit; forget completed records"));
    auto record = std::make_shared<Record>(); record->module_type = module_type->second;
    record->snapshot.mid = new_mid(); record->snapshot.generation = record->snapshot.mid;
    record->snapshot.module_type = module_type_name; record->arguments = std::move(args);
    impl_->records.push_back(record); impl_->wake.notify_all(); return record->snapshot.mid;
}
control::Result<void> ModuleManager::wait_running(const ModuleKey& name, Deadline deadline, std::stop_token stop) {
    auto record = impl_->find(name);
    if (!record) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module"));
    std::unique_lock lock(record->mutex);
    while (!record->user_stop && record->snapshot.state != ModuleState::Running && !terminal(record->snapshot.state) &&
        !record->startup_rejected && matches(*record, name) && !stop.stop_requested() && Clock::now() < deadline)
        record->changed.wait_until(lock, (std::min)(deadline, Clock::now() + 20ms));
    if (!matches(*record, name)) return std::unexpected(failure(control::ErrorCode::Closed, "module MID changed during startup"));
    if (record->startup_rejected) return std::unexpected(failure(control::ErrorCode::InvalidArgument, record->snapshot.last_error));
    if (stop.stop_requested()) return std::unexpected(failure(control::ErrorCode::Closed, "startup wait canceled; process remains supervised"));
    if (!record->user_stop && record->snapshot.state != ModuleState::Running && !terminal(record->snapshot.state))
        return std::unexpected(failure(control::ErrorCode::Timeout, "waiting for module readiness"));
    if (record->user_stop) return std::unexpected(failure(control::ErrorCode::Closed, "module is stopping"));
    if (record->snapshot.state != ModuleState::Running)
        return std::unexpected(control::Error{control::ErrorCode::Closed, record->snapshot.last_error, record->snapshot.os_error});
    return {};
}
control::Result<void> ModuleManager::wait_started(const ModuleKey& name, std::stop_token stop) {
    auto record = impl_->find(name);
    if (!record) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module"));
    return wait_running(name, Clock::now() + record->module_type.policy.startup_timeout + 1s, stop);
}
control::Result<void> ModuleManager::reserve_command_name(const std::string& name) {
    std::lock_guard lock(impl_->mutex);
    if (name.empty() || impl_->closing || impl_->module_types.contains(name) || impl_->programs.contains(name) || impl_->find_locked(name))
        return std::unexpected(failure(control::ErrorCode::InvalidArgument, "command name conflicts with module namespace: " + name));
    for (const auto& [_, module_type] : impl_->module_types)
        if (module_type.module_base_name == name)
            return std::unexpected(failure(control::ErrorCode::InvalidArgument, "reserved command conflicts with module base name: " + name));
    impl_->reserved_commands.insert(name);
    return {};
}
void ModuleManager::begin_shutdown() {
    // Hold admission lock until every existing record has been marked: no start can slip through.
    std::lock_guard lock(impl_->mutex);
    impl_->closing = true;
    for (auto& record : impl_->records) {
        std::lock_guard record_lock(record->mutex);
        record->user_stop = true;
        record->changed.notify_all();
    }
    impl_->wake.notify_all();
}
std::vector<ModuleSnapshot> ModuleManager::snapshots() const {
    std::vector<ModuleSnapshot> result;
    for (auto& record : impl_->snapshot_records()) {
        std::lock_guard lock(record->mutex); result.push_back(record->snapshot);
    }
    return result;
}
// Finish means both OS exit observation and broker cleanup have completed.
// A caller deadline only ends this wait; the supervisor continues stopping children.
control::Result<void> ModuleManager::wait_shutdown(Deadline deadline) {
    begin_shutdown();
    for (auto& record : impl_->snapshot_records()) {
        std::unique_lock lock(record->mutex);
        if (!record->changed.wait_until(lock, deadline, [&] { return terminal(record->snapshot.state) && !record->process; }))
            return std::unexpected(failure(control::ErrorCode::Timeout, "still supervising: " + record->snapshot.name));
    }
    if (!impl_->broker.drain_until(deadline))
        return std::unexpected(failure(control::ErrorCode::Timeout, "waiting for Bus resource cleanup"));
    if (!impl_->output.wait_until(deadline))
        return std::unexpected(failure(control::ErrorCode::Timeout, "waiting for output tail drain"));
    return {};
}
control::Result<void> ModuleManager::stop_module(const ModuleKey& name, Deadline deadline) {
    auto record = impl_->find(name);
    if (!record) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module"));
    std::unique_lock lock(record->mutex);
    if (!matches(*record, name)) return std::unexpected(failure(control::ErrorCode::Closed, "stale module MID"));
    record->user_stop = true; impl_->wake.notify_all();
    if (!record->changed.wait_until(lock, deadline, [&] { return terminal(record->snapshot.state); }))
        return std::unexpected(failure(control::ErrorCode::Timeout, "stop pending; process remains supervised"));
    return {};
}
control::Result<void> ModuleManager::forget_module(const ModuleKey& name) {
    std::lock_guard lock(impl_->mutex); auto record = impl_->find_locked(name);
    if (!record) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module"));
    std::lock_guard record_lock(record->mutex);
    if (!matches(*record, name)) return std::unexpected(failure(control::ErrorCode::Closed, "stale module MID"));
    if (!terminal(record->snapshot.state)) return std::unexpected(failure(control::ErrorCode::Backpressure, "module still supervised"));
    std::erase(impl_->records, record); return {};
}
std::optional<ModuleSnapshot> ModuleManager::inspect(const ModuleKey& name) const {
    auto record = impl_->find(name); if (!record) return {};
    std::lock_guard lock(record->mutex); if (!matches(*record, name)) return {}; return record->snapshot;
}
control::Result<int32_t> ModuleManager::command(const ModuleKey& name, std::vector<std::string> args, Deadline deadline, std::stop_token stop) {
    auto response = impl_->request(name, detail::execute, std::move(args), deadline, stop);
    if (!response) return std::unexpected(response.error());
    control::Reader reader(response->payload); const auto code = reader.u32();
    if (!reader.done()) return std::unexpected(failure(control::ErrorCode::Protocol, "invalid command response"));
    return std::bit_cast<int32_t>(code);
}
int8_t ModuleManager::unregister_module_type(const std::string& name) {
    std::lock_guard lock(impl_->mutex); return impl_->module_types.erase(name) ? 0 : -1;
}
control::Result<std::vector<std::string>> ModuleManager::complete(const ModuleKey& name, std::vector<std::string> args, Deadline deadline) {
    auto response = impl_->request(name, detail::complete, std::move(args), deadline);
    if (!response) return std::unexpected(response.error());
    if (response->payload.size() < 4) return std::unexpected(failure(control::ErrorCode::Protocol, "short completion response"));
    control::Reader status(std::span<const std::byte>(response->payload).first(4));
    if (status.u32()) return std::unexpected(failure(control::ErrorCode::Protocol, "module completion hook failed"));
    auto decoded = control::decode_arguments(std::span<const std::byte>(response->payload).subspan(4));
    if (!decoded) return std::unexpected(failure(control::ErrorCode::Protocol, decoded.error()));
    return std::move(*decoded);
}
int32_t ModuleManager::call_module_type_interface(const std::string& name, std::vector<std::string> args) {
    auto started = start_module(name, std::move(args));
    if (!started) return -1;
    // Preserve the Shell's start-then-command sequencing. Advanced callers can
    // use start_module/inspect for asynchronous startup instead.
    return wait_started(*started) ? 0 : -1;
}
int32_t ModuleManager::call_module_interface(const std::string& name, std::vector<std::string> args) {
    auto response = command(name, std::move(args), Clock::now() + 5s); return response ? *response : -1;
}
std::vector<std::string> ModuleManager::call_module_type_autoComplete(const std::string&, std::vector<std::string>) { return {}; }
std::vector<std::string> ModuleManager::call_module_autoComplete(const std::string& name, std::vector<std::string> args) {
    auto response = impl_->request(name, detail::complete, std::move(args), Clock::now() + 1s);
    if (!response || response->payload.size() < 4) return {};
    control::Reader status(std::span<const std::byte>(response->payload).first(4));
    if (status.u32() != 0) return {};
    auto decoded = control::decode_arguments(std::span<const std::byte>(response->payload).subspan(4));
    return decoded ? std::move(*decoded) : std::vector<std::string>{};
}
int8_t ModuleManager::register_micro_program(const std::string& name, CommandFunction main, AutoCompleteFunction completion) {
    std::lock_guard lock(impl_->mutex);
    if (name.empty() || impl_->closing || impl_->reserved_commands.contains(name) || impl_->programs.contains(name) || impl_->module_types.contains(name) || impl_->find_locked(name)) return -1;
    impl_->programs.emplace(name, microProgram{name, std::move(main), std::move(completion)}); return 0;
}
int32_t ModuleManager::call_micro_program(const std::string& name, std::vector<std::string> args) {
    CommandFunction function;
    { std::lock_guard lock(impl_->mutex); auto it = impl_->programs.find(name); if (it != impl_->programs.end()) function = it->second.main_func; }
    return function ? function(args) : ERR_MICRO_PROGRAM_NOT_FOUND;
}
std::vector<std::string> ModuleManager::call_micro_program_autoComplete(const std::string& name, std::vector<std::string> args) {
    AutoCompleteFunction function;
    { std::lock_guard lock(impl_->mutex); auto it = impl_->programs.find(name); if (it != impl_->programs.end()) function = it->second.autoComplete_func; }
    return function ? function(args) : std::vector<std::string>{};
}
std::vector<std::string> ModuleManager::get_registered_module_type_names() const {
    std::lock_guard lock(impl_->mutex); std::vector<std::string> names;
    for (const auto& [name, _] : impl_->module_types) names.push_back(name); return names;
}
std::vector<std::string> ModuleManager::get_active_module_names() const {
    std::vector<std::string> names;
    for (auto& record : impl_->snapshot_records()) { std::lock_guard lock(record->mutex); if (!record->snapshot.name.empty() && !terminal(record->snapshot.state)) names.push_back(record->snapshot.name); }
    return names;
}
std::vector<std::string> ModuleManager::get_micro_program_names() const {
    std::lock_guard lock(impl_->mutex); std::vector<std::string> names;
    for (const auto& [name, _] : impl_->programs) names.push_back(name); return names;
}
bool ModuleManager::has_module_type(const std::string& name) const { std::lock_guard lock(impl_->mutex); return impl_->module_types.contains(name); }
bool ModuleManager::has_active_module(const std::string& name) const { auto s = inspect(name); return s && !terminal(s->state); }
bool ModuleManager::has_micro_program(const std::string& name) const { std::lock_guard lock(impl_->mutex); return impl_->programs.contains(name); }
}

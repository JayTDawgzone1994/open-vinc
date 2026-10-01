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
std::atomic<std::uint64_t> next_generation{1};
struct Record {
    std::mutex mutex;
    std::condition_variable changed;
    ModuleDefinition definition;
    ModuleSnapshot snapshot;
    std::vector<std::string> arguments;
    std::optional<osal::Process> process;
    std::optional<osal::Socket> listener;
    std::filesystem::path socket_path;
    std::shared_ptr<control::Session> session;
    std::string token;
    Deadline startup_deadline{}, stop_deadline{}, restart_at{}, next_terminate{};
    bool user_stop = false, fault = false, stop_sent = false;
    bool pending_waiter = false;
    std::uint64_t next_request = 1, pending_request = 0;
    std::uint16_t pending_operation = 0;
    std::optional<control::Frame> response;
};
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
    std::map<std::string, ModuleDefinition> definitions;
    std::map<std::string, std::shared_ptr<Record>> records;
    std::map<std::string, microProgram> programs;
    std::set<std::string> reserved_commands;
    std::unique_ptr<control::Service> channels;
    osal::ProcessService processes;
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
    std::shared_ptr<Record> find(const std::string& name) const {
        std::lock_guard lock(mutex); auto it = records.find(name); return it == records.end() ? nullptr : it->second;
    }
    std::vector<std::shared_ptr<Record>> snapshot_records() const {
        std::lock_guard lock(mutex); std::vector<std::shared_ptr<Record>> out;
        for (const auto& [_, record] : records) out.push_back(record); return out;
    }
    void launch(Record& record) {
        record.snapshot.generation = next_generation.fetch_add(1);
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
        const auto& policy = record.definition.policy;
        osal::SpawnOptions options;
        options.new_process_group = true; // central coordinates stop; terminal Ctrl+C must not kill children first
        options.executable = record.definition.executable;
        options.arguments = {"--vinc-module-v1", utf8(record.socket_path), record.snapshot.name, record.token,
            std::to_string(record.snapshot.generation), std::to_string(policy.heartbeat_interval.count()),
            std::to_string(policy.heartbeat_timeout.count()), std::to_string(policy.startup_timeout.count()),
            std::to_string(policy.stop_timeout.count()), record.definition.name};
        options.arguments.insert(options.arguments.end(), record.arguments.begin(), record.arguments.end());
        record.startup_deadline = Clock::now() + policy.startup_timeout;
        auto child = processes.spawn(options);
        if (!child) { record_failure(record, "spawn module", child.error()); return; }
        record.process.emplace(std::move(*child)); record.snapshot.pid = record.process->pid();
        record.snapshot.state = ModuleState::Starting; record.changed.notify_all();
    }
    void finished(Record& record) {
        if (record.session) record.session->close(); record.session.reset(); close_listener(record);
        if (record.process) { (void)processes.forget(record.process->id()); record.process.reset(); }
        record.pending_request = 0; record.response.reset();
        if (!record.user_stop && record.snapshot.restarts < record.definition.policy.max_restarts) {
            ++record.snapshot.restarts; record.restart_at = Clock::now() + record.definition.policy.restart_delay;
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
                broker.exited(record.snapshot.generation);
                if (!record.user_stop && !record.fault) record.snapshot.last_error = "module exited unexpectedly";
                finished(record); return;
            }
        }
        if (record.fault && !record.process) { finished(record); return; }
        if (record.user_stop || record.fault) {
            if (record.snapshot.state != ModuleState::Stopping) {
                record.snapshot.state = ModuleState::Stopping;
                record.stop_deadline = now + record.definition.policy.stop_timeout;
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
                options.heartbeat_interval = record.definition.policy.heartbeat_interval;
                options.heartbeat_timeout = record.definition.policy.heartbeat_timeout;
                options.handshake_timeout = record.definition.policy.startup_timeout;
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
                    if (!broker.submit(record.snapshot.generation, record.snapshot.name, record.session, std::move(frame))) {
                        record_failure(record, "Bus control queue exhausted"); break;
                    }
                } else if (frame.kind == control::Kind::Event && frame.domain == detail::lifecycle && frame.operation == detail::ready) {
                    control::Reader reader(frame.payload); const auto name = reader.string();
                    if (name != record.snapshot.name || !reader.done() || record.snapshot.state != ModuleState::Starting) {
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
    void monitor(std::stop_token token) {
        while (!token.stop_requested()) {
            for (auto& record : snapshot_records()) {
                std::lock_guard lock(record->mutex);
                try { tick(*record); }
                catch (const std::exception& e) { record_failure(*record, e.what()); }
                catch (...) { record_failure(*record, "module supervisor exception"); }
            }
            std::unique_lock lock(mutex); wake.wait_for(lock, 20ms);
        }
    }
    control::Result<control::Frame> request(const std::string& name, std::uint16_t operation,
        std::vector<std::string> args, Deadline deadline) {
        auto record = find(name);
        if (!record) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module"));
        control::Bytes payload;
        try { payload = control::encode_arguments(args); }
        catch (const std::length_error& e) { return std::unexpected(failure(control::ErrorCode::InvalidArgument, e.what())); }
        std::unique_lock lock(record->mutex);
        if (record->snapshot.state != ModuleState::Running || record->user_stop)
            return std::unexpected(failure(control::ErrorCode::Closed, "module is not running"));
        if (record->pending_request) return std::unexpected(failure(control::ErrorCode::Backpressure, "module already has an outstanding request"));
        if (Clock::now() >= deadline) return std::unexpected(failure(control::ErrorCode::Timeout, "request deadline expired"));
        if (!record->next_request) return std::unexpected(failure(control::ErrorCode::Protocol, "request IDs exhausted"));
        const auto id = record->next_request++; const auto generation = record->snapshot.generation;
        record->pending_request = id; record->pending_operation = operation; record->response.reset(); record->pending_waiter = true;
        auto sent = record->session->send({control::Kind::Request, detail::commands, operation, id, std::move(payload)});
        if (!sent) { record->pending_request = 0; return std::unexpected(sent.error()); }
        record->changed.wait_until(lock, deadline, [&] {
            return record->response.has_value() || record->user_stop || record->snapshot.state != ModuleState::Running || record->snapshot.generation != generation;
        });
        if (record->snapshot.generation != generation)
            return std::unexpected(failure(control::ErrorCode::Closed, "module generation changed; command outcome may be unknown"));
        record->pending_waiter = false;
        if (record->user_stop || record->snapshot.state != ModuleState::Running)
            return std::unexpected(failure(control::ErrorCode::Closed, "module session ended; command outcome may be unknown"));
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
        deadline = (std::max)(deadline, Clock::now() + record->definition.policy.stop_timeout + 2s);
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
    std::error_code ec; std::filesystem::remove(impl_->directory, ec);
}
ModuleManager& ModuleManager::get() { static ModuleManager manager; return manager; }
control::Result<void> ModuleManager::register_module(ModuleDefinition definition) {
    const auto& p = definition.policy;
    if (definition.name.empty() || definition.name.size() > 128 || definition.name.find('\0') != std::string::npos ||
        definition.node_base_name.size() > 128 || definition.node_base_name.find('\0') != std::string::npos ||
        !definition.executable.is_absolute() || p.heartbeat_interval <= 0ms || p.heartbeat_timeout <= p.heartbeat_interval ||
        p.heartbeat_timeout > 24h || p.startup_timeout <= 0ms || p.startup_timeout > 24h ||
        p.stop_timeout <= 0ms || p.stop_timeout > 60s || p.restart_delay < 100ms || p.max_restarts > 1000)
        return std::unexpected(failure(control::ErrorCode::InvalidArgument, "invalid module definition or policy"));
    if (definition.node_base_name.empty()) definition.node_base_name = definition.name;
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->reserved_commands.contains(definition.name) || impl_->definitions.contains(definition.name) || impl_->programs.contains(definition.name) || impl_->records.contains(definition.name))
        return std::unexpected(failure(control::ErrorCode::InvalidArgument, "module factory name already registered or manager closing"));
    impl_->definitions.emplace(definition.name, std::move(definition)); return {};
}
control::Result<std::string> ModuleManager::start_module(const std::string& factory, std::vector<std::string> args) {
    try { (void)control::encode_arguments(args); }
    catch (const std::length_error& e) { return std::unexpected(failure(control::ErrorCode::InvalidArgument, e.what())); }
    std::lock_guard lock(impl_->mutex); auto definition = impl_->definitions.find(factory);
    if (impl_->closing || definition == impl_->definitions.end()) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown factory or closing manager"));
    auto name = definition->second.node_base_name;
    for (std::size_t suffix = 0;; ++suffix) {
        name = definition->second.node_base_name + (suffix ? "_" + std::to_string(suffix) : "");
        if (impl_->reserved_commands.contains(name) || impl_->definitions.contains(name) || impl_->programs.contains(name)) continue;
        auto existing = impl_->records.find(name);
        if (existing == impl_->records.end()) break;
        auto previous = existing->second;
        std::lock_guard record_lock(previous->mutex);
        if (terminal(previous->snapshot.state)) { impl_->records.erase(existing); break; }
    }
    if (impl_->records.size() >= 256) return std::unexpected(failure(control::ErrorCode::Backpressure, "module record limit; forget completed records"));
    auto record = std::make_shared<Record>(); record->definition = definition->second;
    record->arguments = std::move(args); record->snapshot.name = name;
    impl_->records.emplace(name, record); impl_->wake.notify_all(); return name;
}
control::Result<void> ModuleManager::wait_running(const std::string& name, Deadline deadline) {
    auto record = impl_->find(name);
    if (!record) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module"));
    std::unique_lock lock(record->mutex);
    if (!record->changed.wait_until(lock, deadline, [&] { return record->user_stop || record->snapshot.state == ModuleState::Running || terminal(record->snapshot.state); }))
        return std::unexpected(failure(control::ErrorCode::Timeout, "waiting for module readiness"));
    if (record->user_stop) return std::unexpected(failure(control::ErrorCode::Closed, "module is stopping"));
    if (record->snapshot.state != ModuleState::Running)
        return std::unexpected(control::Error{control::ErrorCode::Closed, record->snapshot.last_error, record->snapshot.os_error});
    return {};
}
control::Result<void> ModuleManager::wait_started(const std::string& name) {
    auto record = impl_->find(name);
    if (!record) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module"));
    return wait_running(name, Clock::now() + record->definition.policy.startup_timeout + 1s);
}
control::Result<void> ModuleManager::reserve_command_name(const std::string& name) {
    std::lock_guard lock(impl_->mutex);
    if (name.empty() || impl_->closing || impl_->definitions.contains(name) || impl_->programs.contains(name) || impl_->records.contains(name))
        return std::unexpected(failure(control::ErrorCode::InvalidArgument, "command name conflicts with module namespace: " + name));
    impl_->reserved_commands.insert(name);
    return {};
}
void ModuleManager::begin_shutdown() {
    // Hold admission lock until every existing record has been marked: no start can slip through.
    std::lock_guard lock(impl_->mutex);
    impl_->closing = true;
    for (auto& [_, record] : impl_->records) {
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
control::Result<void> ModuleManager::wait_shutdown(Deadline deadline) {
    begin_shutdown();
    for (auto& record : impl_->snapshot_records()) {
        std::unique_lock lock(record->mutex);
        if (!record->changed.wait_until(lock, deadline, [&] { return terminal(record->snapshot.state) && !record->process; }))
            return std::unexpected(failure(control::ErrorCode::Timeout, "still supervising: " + record->snapshot.name));
    }
    if (!impl_->broker.drain_until(deadline))
        return std::unexpected(failure(control::ErrorCode::Timeout, "waiting for Bus resource cleanup"));
    return {};
}
control::Result<void> ModuleManager::stop_module(const std::string& name, Deadline deadline) {
    auto record = impl_->find(name);
    if (!record) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module"));
    std::unique_lock lock(record->mutex); record->user_stop = true; impl_->wake.notify_all();
    if (!record->changed.wait_until(lock, deadline, [&] { return terminal(record->snapshot.state); }))
        return std::unexpected(failure(control::ErrorCode::Timeout, "stop pending; process remains supervised"));
    return {};
}
control::Result<void> ModuleManager::forget_module(const std::string& name) {
    std::lock_guard lock(impl_->mutex); auto it = impl_->records.find(name);
    if (it == impl_->records.end()) return std::unexpected(failure(control::ErrorCode::InvalidArgument, "unknown module"));
    auto record = it->second; std::lock_guard record_lock(record->mutex);
    if (!terminal(record->snapshot.state)) return std::unexpected(failure(control::ErrorCode::Backpressure, "module still supervised"));
    impl_->records.erase(it); return {};
}
std::optional<ModuleSnapshot> ModuleManager::inspect(const std::string& name) const {
    auto record = impl_->find(name); if (!record) return {};
    std::lock_guard lock(record->mutex); return record->snapshot;
}
control::Result<int32_t> ModuleManager::command(const std::string& name, std::vector<std::string> args, Deadline deadline) {
    auto response = impl_->request(name, detail::execute, std::move(args), deadline);
    if (!response) return std::unexpected(response.error());
    control::Reader reader(response->payload); const auto code = reader.u32();
    if (!reader.done()) return std::unexpected(failure(control::ErrorCode::Protocol, "invalid command response"));
    return std::bit_cast<int32_t>(code);
}
int8_t ModuleManager::unregister_module_factory(const std::string& name) {
    std::lock_guard lock(impl_->mutex); return impl_->definitions.erase(name) ? 0 : -1;
}
control::Result<std::vector<std::string>> ModuleManager::complete(const std::string& name, std::vector<std::string> args, Deadline deadline) {
    auto response = impl_->request(name, detail::complete, std::move(args), deadline);
    if (!response) return std::unexpected(response.error());
    if (response->payload.size() < 4) return std::unexpected(failure(control::ErrorCode::Protocol, "short completion response"));
    control::Reader status(std::span<const std::byte>(response->payload).first(4));
    if (status.u32()) return std::unexpected(failure(control::ErrorCode::Protocol, "module completion hook failed"));
    auto decoded = control::decode_arguments(std::span<const std::byte>(response->payload).subspan(4));
    if (!decoded) return std::unexpected(failure(control::ErrorCode::Protocol, decoded.error()));
    return std::move(*decoded);
}
int32_t ModuleManager::call_module_interface(const std::string& name, std::vector<std::string> args) {
    auto started = start_module(name, std::move(args));
    if (!started) return -1;
    auto record = impl_->find(*started);
    // Preserve the Shell's start-then-command sequencing. Advanced callers can
    // use start_module/inspect for asynchronous startup instead.
    const auto deadline = Clock::now() + record->definition.policy.startup_timeout + 1s;
    return wait_running(*started, deadline) ? 0 : -1;
}
int32_t ModuleManager::call_node_interface(const std::string& name, std::vector<std::string> args) {
    auto response = command(name, std::move(args), Clock::now() + 5s); return response ? *response : -1;
}
std::vector<std::string> ModuleManager::call_module_autoComplete(const std::string&, std::vector<std::string>) { return {}; }
std::vector<std::string> ModuleManager::call_node_autoComplete(const std::string& name, std::vector<std::string> args) {
    auto response = impl_->request(name, detail::complete, std::move(args), Clock::now() + 1s);
    if (!response || response->payload.size() < 4) return {};
    control::Reader status(std::span<const std::byte>(response->payload).first(4));
    if (status.u32() != 0) return {};
    auto decoded = control::decode_arguments(std::span<const std::byte>(response->payload).subspan(4));
    return decoded ? std::move(*decoded) : std::vector<std::string>{};
}
int8_t ModuleManager::register_micro_program(const std::string& name, CommandFunction main, AutoCompleteFunction completion) {
    std::lock_guard lock(impl_->mutex);
    if (name.empty() || impl_->closing || impl_->reserved_commands.contains(name) || impl_->programs.contains(name) || impl_->definitions.contains(name) || impl_->records.contains(name)) return -1;
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
std::vector<std::string> ModuleManager::get_registered_factory_names() const {
    std::lock_guard lock(impl_->mutex); std::vector<std::string> names;
    for (const auto& [name, _] : impl_->definitions) names.push_back(name); return names;
}
std::vector<std::string> ModuleManager::get_active_node_names() const {
    std::vector<std::string> names;
    for (auto& record : impl_->snapshot_records()) { std::lock_guard lock(record->mutex); if (!terminal(record->snapshot.state)) names.push_back(record->snapshot.name); }
    return names;
}
std::vector<std::string> ModuleManager::get_micro_program_names() const {
    std::lock_guard lock(impl_->mutex); std::vector<std::string> names;
    for (const auto& [name, _] : impl_->programs) names.push_back(name); return names;
}
bool ModuleManager::has_factory(const std::string& name) const { std::lock_guard lock(impl_->mutex); return impl_->definitions.contains(name); }
bool ModuleManager::has_active_node(const std::string& name) const { auto s = inspect(name); return s && !terminal(s->state); }
bool ModuleManager::has_micro_program(const std::string& name) const { std::lock_guard lock(impl_->mutex); return impl_->programs.contains(name); }
}

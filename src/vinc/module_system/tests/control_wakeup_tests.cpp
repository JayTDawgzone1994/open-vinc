#include "plugin_test_support.hpp"
#include "control_wakeup_trace.hpp"
// Link the real central implementation with bounded observation gates. The
// ordinary Manager and Control Channel libraries contain no active test hooks.
#include "../src/host/module_manager.cpp"

namespace {
using namespace vinc;
using vinc::test::plugin::check;
using vinc::test::plugin::take;
namespace probe = vinc::test::control_wakeup;
constexpr auto gate_budget = std::chrono::seconds(8);
constexpr int controlled_samples = 5;
constexpr int callback_samples = 64;
constexpr std::uint64_t first_probe_request = 1000000;

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<control::Session> session;
    bool armed = false;
    bool entered = false;
    bool released = false;
    bool queued = false;
    bool taken = false;
    bool wait_finished = false;
    bool expired = false;
    std::uint64_t request = 0;
    std::cv_status wait_status = std::cv_status::no_timeout;
    probe::Clock::time_point queued_at;
    probe::Clock::time_point wait_started_at;
    probe::Clock::time_point wait_finished_at;
    probe::Clock::time_point taken_at;
} gate;

class ReleaseGate {
public:
    ~ReleaseGate() {
        std::lock_guard lock(gate.mutex);
        gate.released = true;
        gate.armed = false;
        gate.changed.notify_all();
    }
};

template<class Predicate>
void await(std::unique_lock<std::mutex>& lock, Predicate predicate, const char* reason) {
    check(gate.changed.wait_for(lock, gate_budget, predicate), reason);
    check(!gate.expired, "supervisor test gate expired");
}

std::shared_ptr<control::Session> arm(std::uint64_t request) {
    std::unique_lock lock(gate.mutex);
    gate.entered = false;
    gate.released = false;
    gate.queued = false;
    gate.taken = false;
    gate.wait_finished = false;
    gate.request = request;
    gate.armed = true;
    await(lock, [] { return gate.entered; }, "supervisor reached pre-wait gate");
    check(bool(gate.session), "central Session captured");
    return gate.session;
}

void send_probe(control::Session& session, std::uint64_t request) {
    // No Manager API is called while supervision is gated. The real Module
    // returns its ordinary empty-command response; unmatched replies are legal.
    control::Frame frame{control::Kind::Request, detail::commands, detail::execute, request, {}};
    frame.payload = take(control::encode_arguments({}));
    auto sent = session.send(std::move(frame));
    check(bool(sent), "probe command enqueued for real Module");
}

bool delayed_receive(std::uint64_t request, std::ostream& report) {
    ReleaseGate release_on_error;
    auto session = arm(request);
    send_probe(*session, request);

    std::unique_lock lock(gate.mutex);
    await(lock, [] { return gate.queued; }, "reply entered central inbox before wait");
    check(!gate.taken, "supervisor has not consumed gated reply");
    // Queued is observed inside Peer::mutex before notification. Synchronize
    // with the end of that publication, without holding the test gate mutex.
    lock.unlock();
    check(session->state() == control::State::Open, "reply publication completed");
    lock.lock();
    gate.released = true;
    gate.changed.notify_all();
    await(lock, [] { return gate.wait_finished && gate.taken; }, "supervisor eventually consumed reply");
    check(gate.queued_at <= gate.wait_started_at &&
          gate.wait_started_at <= gate.wait_finished_at &&
          gate.wait_finished_at <= gate.taken_at, "queue/wait/dequeue causal order");
    const auto wait_ms = std::chrono::duration<double, std::milli>(
        gate.wait_finished_at - gate.wait_started_at).count();
    const auto queue_ms = std::chrono::duration<double, std::milli>(
        gate.taken_at - gate.queued_at).count();
    const bool timed_out = gate.wait_status == std::cv_status::timeout;
    report << "controlled request=" << request << " wait_timeout=" << timed_out
           << " wait_ms=" << wait_ms << " queue_ms=" << queue_ms << '\n';
    return timed_out;
}

void direct_receive() {
    ReleaseGate release_on_error;
    const auto request = first_probe_request + controlled_samples;
    auto session = arm(request);
    send_probe(*session, request);
    // This wait uses Peer::changed, not Manager::wake. No manual wake or sleep
    // helps either side. The supervisor remains gated throughout reception.
    auto received = take(session->receive_until(probe::Clock::now() + gate_budget));
    check(received.kind == control::Kind::Response && received.request == request,
          "receive_until independently receives the matching reply");
    std::lock_guard lock(gate.mutex);
    check(gate.entered && !gate.released, "direct receiver works while supervisor is gated");
}

void set_trace_path(const std::filesystem::path& path) {
    const auto text = take(osal::path_to_utf8(path));
#ifdef _WIN32
    check(_putenv_s("VINC_TEST_CONTROL_TRACE", text.c_str()) == 0, "set child trace environment");
#else
    check(setenv("VINC_TEST_CONTROL_TRACE", text.c_str(), 1) == 0, "set child trace environment");
#endif
}
}

namespace vinc::test::control_wakeup {
void pass_started(bool, Deadline) {}
void before_rpc_submit(rpc::detail::RpcManager&, std::uint64_t, const control::Frame&) {}
void after_rpc_submit(std::uint64_t, const control::Frame&, const Result<void>&) {}
void frame_event(Point point, control::Role role, std::uint64_t generation,
                 const control::Frame& frame) {
    trace.record(point, role, generation, frame);
    if (role != control::Role::Central || frame.kind != control::Kind::Response ||
        frame.domain != detail::commands) {
        return;
    }

    std::lock_guard lock(gate.mutex);
    if (!gate.armed || frame.request != gate.request) {
        return;
    }
    if (point == Point::Queued) {
        gate.queued = true;
        gate.queued_at = Clock::now();
    } else if (point == Point::Taken) {
        gate.taken = true;
        gate.taken_at = Clock::now();
    }
    gate.changed.notify_all();
}

void attached(const std::shared_ptr<control::Session>& session) {
    std::lock_guard lock(gate.mutex);
    gate.session = session;
}

void before_wait() {
    std::unique_lock lock(gate.mutex);
    if (!gate.armed || gate.entered) {
        return;
    }
    gate.entered = true;
    gate.changed.notify_all();
    if (!gate.changed.wait_for(lock, gate_budget, [] { return gate.released; })) {
        gate.expired = true;
        gate.changed.notify_all();
    }
    gate.wait_started_at = Clock::now();
}

void after_wait(std::cv_status status) {
    std::lock_guard lock(gate.mutex);
    if (gate.armed && gate.entered && gate.released && !gate.wait_finished) {
        gate.wait_finished = true;
        gate.wait_status = status;
        gate.wait_finished_at = Clock::now();
        gate.changed.notify_all();
    }
}
}

int main(int argc, char** argv) {
    try {
        check(argc >= 3 && argc <= 5, "arguments: Host Plugin [output-directory] [--require-wakeup]");
        const bool require_wakeup = argc == 5 && std::string_view(argv[4]) == "--require-wakeup";
        check(argc != 5 || require_wakeup, "unknown regression mode");
        test::plugin::Scope scope;
        ReleaseGate release_before_manager_cleanup;
        const auto output = argc >= 4 ? take(osal::path_from_utf8(argv[3])) : scope.directory;
        std::filesystem::create_directories(output);
        set_trace_path(output / "child.csv");
        scope.create(take(osal::path_from_utf8(argv[1])));
        ModuleType type;
        type.name = "messageBusTest";
        type.library = take(osal::path_from_utf8(argv[2]));
        check(bool(scope.manager->register_module_type(std::move(type))), "register real Bus Plugin");
        const auto mid = take(scope.manager->start_module("messageBusTest", {"sub", "wakeup/initial"}));
        auto ready = scope.manager->wait_running(mid, probe::Clock::now() + gate_budget);
        if (!ready) {
            std::cerr << "Ready MID=" << mid << ": " << error_to_string(ready.error()) << '\n';
            const auto snapshot = scope.manager->inspect(mid);
            if (snapshot && snapshot->last_failure) {
                std::cerr << "Primary: " << error_to_string(snapshot->last_failure->error) << '\n';
            }
            if (snapshot && snapshot->last_exit) {
                std::cerr << "Exit: " << snapshot->last_exit->value << '\n';
            }
            probe::trace.write(output / "parent-failure.csv");
            check(false, "Module ready");
        }

        std::ofstream evidence(output / "evidence.txt");
        int timeout_count = 0;
        for (int index = 0; index < controlled_samples; ++index) {
            timeout_count += delayed_receive(first_probe_request + index, evidence) ? 1 : 0;
        }
        direct_receive();
        evidence << "direct receive_until: PASS while supervisor gated\n";

        // A framework-owned workload, not the external 491-topic application.
        // Each callback registration retains Subscribe + EnableNotifications.
        const auto workload_start = probe::Clock::now();
        for (int index = 0; index < callback_samples; ++index) {
            auto registered = take(scope.manager->command(mid,
                {"hold-callback", "wakeup/topic" + std::to_string(index)},
                probe::Clock::now() + gate_budget));
            check(registered == 0, "callback registration succeeds");
        }
        const auto workload_end = probe::Clock::now();
        evidence << "callbacks=" << callback_samples << " command_workload_ms="
                 << std::chrono::duration<double, std::milli>(workload_end - workload_start).count() << '\n';
        scope.manager->begin_shutdown();
        check(bool(scope.manager->wait_shutdown(probe::Clock::now() + gate_budget)), "confirmed shutdown");
        const auto final = scope.manager->inspect(mid);
        check(final && final->last_exit && final->last_exit->kind == osal::ExitKind::Code &&
              final->last_exit->value == 0, "trace Host exits normally");
        probe::trace.write(output / "parent.csv");
        check(std::filesystem::file_size(output / "child.csv") > 0, "child trace written after teardown");
        evidence << "normal exit and trace collection: PASS\n";
        evidence.close();
        check(bool(evidence), "evidence file written");
        std::cout << std::ifstream(output / "evidence.txt").rdbuf();
        {
            std::lock_guard lock(gate.mutex);
            gate.session.reset();
        }
        check(scope.finish(), "owned processes and temporary files cleaned");

        if (timeout_count != 0) {
            std::cerr << "REGRESSION: queued control reply slept until supervisor fallback ("
                      << timeout_count << '/' << controlled_samples << ")\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 2;
    }
}

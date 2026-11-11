// Compile the actual dispatcher into a disposable Host, keeping its current
// teardown order. The companion Bus translation unit uses the same test hooks.
#include "../src/runtime/module_runtime.cpp"
#include "shutdown_test_hooks.hpp"
#include <vinc/message_bus/detail/protocol.hpp>
#include <fstream>

namespace {
using namespace vinc;
using namespace std::chrono_literals;
constexpr auto gate_budget = 2s;
constexpr int probe_failure_exit = 99;
constexpr std::uint64_t retired_publisher_id = 1;

std::string scenario;
std::ofstream trace;
std::mutex trace_mutex;
std::mutex gate_mutex;
std::condition_variable gate_changed;
bool result_ready = false;
bool admitted = false;
bool joining = false;
bool delivered = false;
std::atomic<bool> receiver_exited{false};
thread_local bool final_teardown = false;
thread_local bool injected = false;

void record(const std::string& text) {
    std::lock_guard lock(trace_mutex);
    trace << text << std::endl;
    if (!trace) {
        std::_Exit(probe_failure_exit);
    }
}

template<class Predicate>
void await_gate(std::unique_lock<std::mutex>& lock, Predicate predicate, const char* name) {
    if (!gate_changed.wait_for(lock, gate_budget, predicate)) {
        record(std::string("gate_timeout=") + name);
        // A probe timeout is never accepted as evidence of the production bug.
        // The parent still owns and reaps this process.
        std::_Exit(probe_failure_exit);
    }
}

bool controlled() {
    return scenario == "during-teardown" || scenario == "before-close" || scenario == "malformed";
}

control::Frame route_notification() {
    control::Frame frame{control::Kind::Event, message_bus::detail::message_bus_domain,
                         message_bus::detail::NotificationRoutesChanged};
    control::put_u64(frame.payload, retired_publisher_id);
    control::put_u32(frame.payload, 0); // Empty destination list for a retired Publisher.
    if (scenario == "malformed") {
        // Leave the routing operation/envelope intact but truncate the count.
        frame.payload.pop_back();
    }
    return frame;
}

int run(std::vector<std::string> args) {
    if (args.size() < detail::bootstrap::argument_count + 3) {
        return probe_failure_exit;
    }
    scenario = args.back();
    auto path = osal::path_from_utf8(args[args.size() - 2]);
    if (!path) {
        return probe_failure_exit;
    }
    trace.open(*path);
    record("scenario=" + scenario);
    record("mid=" + args[detail::bootstrap::generation]);
    // Extra application arguments are ignored by the existing Context fixture.
    // Bootstrap, Plugin loading, hooks, completion exchange and exit are real.
    const auto result = run_module_process(args);
    record("runtime_return=" + std::to_string(result));
    return result;
}
}

namespace vinc::shutdown_test {
void hooks_result(const Result<void>& result) {
    record(result ? "hooks_result=ok" : "hooks_result=" + error_to_string(result.error()));
}

void runtime_result(const Result<void>& result) {
    record(result ? "pre_teardown_result=ok" : "pre_teardown_result=" + error_to_string(result.error()));
    if (!controlled()) {
        return;
    }
    if (!result) {
        record("unexpected_pre_teardown_failure");
        std::_Exit(probe_failure_exit);
    }
    std::unique_lock lock(gate_mutex);
    result_ready = true;
    gate_changed.notify_all();
    if (scenario == "before-close") {
        await_gate(lock, [] { return delivered; }, "delivery-before-teardown");
    } else {
        await_gate(lock, [] { return admitted; }, "admission-before-teardown");
    }
}

void after_finished_check(Result<control::Frame>& frame) {
    if (!controlled()) {
        return;
    }
    std::unique_lock lock(gate_mutex);
    if (!result_ready || admitted) {
        return;
    }
    // Do not replace a real frame or a real transport failure. A normal bounded
    // receive timeout supplies the isolated injection opportunity after Completed.
    if (frame || !frame.error().is(control::ErrorCode::Timeout)) {
        return;
    }
    frame = route_notification();
    injected = true;
    admitted = true;
    record("injected_notification=admitted_after_finished_check");
    gate_changed.notify_all();
    if (scenario != "before-close") {
        // The receiver is released when teardown is ready to join it, not when
        // its resource is closed. Waiting for close would deadlock the fixed order.
        await_gate(lock, [] { return joining; }, "watchdog-join");
    }
}

void bus_result(const Result<void>& result) {
    if (injected || !result) {
        record(result ? "bus_result=ok" : "bus_result=" + error_to_string(result.error()));
    }
    if (injected) {
        std::lock_guard lock(gate_mutex);
        delivered = true;
        gate_changed.notify_all();
    }
}

void before_bus_shutdown() {
    final_teardown = true;
    record("receiver_exited_before_bus_shutdown=" + std::to_string(receiver_exited.load()));
    record("guard=before_bus_shutdown");
}

void before_watchdog_join() {
    record("guard=before_watchdog_join");
    std::lock_guard lock(gate_mutex);
    joining = true;
    gate_changed.notify_all();
}

void watchdog_exited() {
    record("watchdog=exited");
    receiver_exited.store(true);
}

void after_bus_shutdown() {
    record("guard=after_bus_shutdown");
    final_teardown = false;
}

void first_bus_close(std::source_location location) {
    const std::string caller = location.function_name();
    // The thread-local stage distinguishes a Guard close from the receive
    // adapter's error shutdown. Other callers retain their actual source site.
    const bool from_guard = final_teardown && caller.find("shutdown") != std::string::npos;
    record(std::string("first_close=") + (from_guard ? "FinalTeardown" : "Other") +
           " caller=" + caller + " line=" + std::to_string(location.line()));
}

void bus_receive(bool closed, const control::Frame& frame) {
    if (!closed && !injected) {
        return;
    }
    control::Reader reader(frame.payload);
    const auto publisher = reader.u64();
    const auto destinations = reader.u32();
    const bool valid_empty_route = frame.kind == control::Kind::Event &&
        frame.domain == message_bus::detail::message_bus_domain &&
        frame.operation == message_bus::detail::NotificationRoutesChanged &&
        frame.request == 0 && publisher != 0 && destinations == 0 && reader.done();
    record("bus_receive closed=" + std::to_string(closed) +
           " injected=" + std::to_string(injected) +
           " valid_empty_route=" + std::to_string(valid_empty_route) +
           " operation=" + std::to_string(frame.operation));
}

void watchdog_failure(const Error& error, int exit_code) {
    const auto expected_cause = Error{message_bus::ErrorCode::Protocol,
                                      message_bus::ErrorOperation::Receive}.record();
    const bool expected_chain = error.is(module_system::ErrorCode::BusFailed) &&
        error.is_operation(module_system::ErrorOperation::Receive) &&
        error.causes().size() == 1 && error.causes().front() == expected_cause;
    record("watchdog_bus_protocol=" + std::to_string(expected_chain));
    record("watchdog_exit=" + std::to_string(exit_code) + " error=" + error_to_string(error));
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int index = 1; index < argc; ++index) {
        auto text = vinc::osal::path_to_utf8(std::filesystem::path(argv[index]));
        if (!text) {
            return probe_failure_exit;
        }
        args.push_back(std::move(*text));
    }
    return run(std::move(args));
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif

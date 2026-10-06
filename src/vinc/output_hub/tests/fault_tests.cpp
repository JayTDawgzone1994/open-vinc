// Private fixture compiles the actual Hub implementation without its collector
// thread, allowing deterministic event/rename/completion ordering.
#include "../src/output_hub.cpp"
#include <vinc/output_hub/text_renderer.hpp>
#include <iostream>

namespace {
using namespace vinc;
using namespace vinc::output_hub;
using namespace std::chrono_literals;
constexpr auto test_timeout = 3s;
thread_local std::optional<test::Point> fail_at;
std::mutex injected_mutex;
std::optional<Error> injected_monitor;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    return std::move(*result);
}

void inject_monitor(std::optional<Error> error) {
    std::lock_guard lock(injected_mutex);
    injected_monitor = error;
}
}

namespace vinc::output_hub::test {
void checkpoint(Point point) {
    if (fail_at == point) {
        fail_at.reset();
        throw std::bad_alloc();
    }
}

std::optional<Error> monitor_error(std::optional<Error> actual) {
    std::lock_guard lock(injected_mutex);
    return injected_monitor ? injected_monitor : actual;
}
}

namespace vinc::output_hub::detail {
struct OutputHubTestAccess {
    static auto idle() {
        Limits limits;
        auto service = take(OutputHub::Impl::create_pipe_service(limits));
        auto impl = std::make_unique<OutputHub::Impl>(limits, std::move(service));
        return std::unique_ptr<OutputHub>(new OutputHub(std::move(impl)));
    }

    static auto& implementation(OutputHub& hub) {
        return *hub.impl_;
    }

    static auto capture(OutputHub& hub) {
        auto capture = OutputHub::Impl::allocate_capture();
        check(bool(hub.impl_->register_capture(capture, "old", 1)), "register fixture capture");
        capture->starting = false;
        return capture;
    }
};
}

namespace {
using Access = vinc::output_hub::detail::OutputHubTestAccess;

void check_gap_and_rename() {
    auto hub = Access::idle();
    auto& impl = Access::implementation(*hub);
    auto capture = Access::capture(*hub);
    auto& reader = capture->readers[0];
    auto first = take(hub->subscribe());
    auto second = take(hub->subscribe());
    auto muted = take(hub->subscribe());
    muted.set_enabled(false);
    (void)muted.take();
    TextRenderer renderer;
    const auto now = std::chrono::steady_clock::now();

    impl.publish(reader, EventKind::Data, "consumed");
    check(renderer.render(first.take(), now).empty(), "hold an unfinished display fragment");
    impl.publish(reader, EventKind::Data, "before");
    fail_at = test::Point::Publish;
    impl.publish(reader, EventKind::Data, "lost");
    impl.publish(reader, EventKind::Data, "after");
    impl.publish(reader, EventKind::Data, "\n");
    const auto partial = first.take(1);
    check(partial.skipped.events == 2 && partial.skipped.bytes == 10, "count queued plus failed event");
    check(partial.events.size() == 1 && partial.events[0]->bytes == "after", "pre-gap queue discarded");
    auto text = renderer.render(partial, now);
    text += renderer.render(first.take(1), now);
    check(text.find("beforeafter") == std::string::npos && text.find("consumedafter") == std::string::npos,
          "never concatenate across gap or across batches");
    check(text.find("after\n") != std::string::npos, "new output survives gap");
    const auto other = second.take();
    check(other.skipped.events == 3 && other.skipped.bytes == 18, "each subscriber counts its own backlog");
    const auto disabled = muted.take();
    check(disabled.events.empty() && disabled.skipped.events == 0 && disabled.skipped.bytes == 0,
          "disabled subscriber does not count unreceived data");

    impl.publish(reader, EventKind::Data, "old fragment");
    check(renderer.render(first.take(), now).empty(), "hold old-label fragment");
    impl.publish(reader, EventKind::Data, "queued old");
    fail_at = test::Point::Publish;
    hub->set_name(capture->id, "new");
    impl.publish(reader, EventKind::Data, "new text\n");
    text = renderer.render(first.take(), now);
    check(text.find("[new#1 stdout] new text") != std::string::npos, "lost rename cannot retain old label");
    check(text.find("old fragment") == std::string::npos, "lost rename resets unfinished display");

    impl.publish(reader, EventKind::Data, "tail");
    fail_at = test::Point::Publish;
    impl.finish(reader, EventKind::Failed, Error{ErrorCode::ReadFailed, ErrorOperation::ReadOutput});
    const auto failed = first.take();
    check(failed.events.empty() && failed.skipped.events == 2 && failed.skipped.bytes == 4,
          "lost metadata also clears pre-gap queue");
    check(hub->stats().failed_streams == 1, "failure counter survives event allocation failure");
}

void check_service_diagnostics() {
    auto hub = Access::idle();
    check(!hub->service_error(), "healthy hub has no diagnostic");
    const Error original{osal::ErrorCode::SystemError, osal::ErrorOperation::WaitIo,
                         {NativeErrorSource::Win32, 123}};
    inject_monitor(original);
    const auto stored = hub->service_error();
    check(stored && stored->is(ErrorCode::ServiceFailed) && stored->is_operation(ErrorOperation::MonitorOutput),
          "service fault uses OutputHub domain and operation");
    check(stored->causes().size() == 1 && stored->causes()[0].domain == ErrorDomain::Osal &&
          stored->native == original.native, "preserve native monitor cause without active capture");
    inject_monitor(Error{osal::ErrorCode::ServiceClosed, osal::ErrorOperation::WaitIo});
    check(hub->service_error() == stored, "subsequent closure cannot replace original failure");
    check(error_to_string(*stored).find("MonitorOutput") != std::string::npos, "catalog includes monitor operation");
    auto processes = take(osal::ProcessService::create());
    osal::SpawnOptions options;
    options.executable = std::filesystem::absolute("must-not-launch");
    auto rejected = hub->spawn(processes, options, "rejected", 2);
    check(!rejected && rejected.error().is(ErrorCode::SpawnFailed) && rejected.error().native == original.native,
          "new spawn retains stored service failure");
    check(hub->stats().active_captures == 0, "rejected capture rolls back admission");
    check(bool(hub->wait_until(std::chrono::steady_clock::now())), "drain success independent of historical fault");
    inject_monitor({});
}

void check_failed_completion() {
    auto hub = Access::idle();
    auto& impl = Access::implementation(*hub);
    auto capture = Access::capture(*hub);
    auto destination = take(hub->subscribe());
    auto independent_capture = Access::capture(*hub);
    const Error original{osal::ErrorCode::SystemError, osal::ErrorOperation::WaitIo,
                         {NativeErrorSource::Win32, 123}};
    inject_monitor(original);
    osal::IoCompletion completion;
    completion.status = osal::IoStatus::Closed;
    completion.buffer = osal::IoBuffer(1);
    completion.buffer.bytes()[0] = std::byte{'x'};
    completion.transferred = 1;
    impl.complete_reader(capture->readers[0], completion);
    auto batch = destination.take();
    check(batch.events.size() == 2 && batch.events[0]->bytes == "x", "confirmed bytes delivered before failure");
    const auto& failed = *batch.events[1];
    check(failed.kind == EventKind::Failed && failed.error && failed.error->is(ErrorCode::ReadFailed),
          "closure after monitor failure is not EOF");
    check(failed.error->native == original.native && failed.error->causes().size() == 2,
          "read event retains service and native causes");

    const auto submission = impl.capture_failure(Error{osal::ErrorCode::ServiceClosed,
                                                      osal::ErrorOperation::ReadPipe});
    check(submission.native == original.native && submission.causes().size() == 2,
          "closed submission retains monitor diagnostic");

    osal::IoCompletion independent;
    independent.status = osal::IoStatus::Failed;
    independent.error = Error{osal::ErrorCode::SystemError, osal::ErrorOperation::ReadPipe,
                              {NativeErrorSource::Win32, 456}};
    impl.complete_reader(independent_capture->readers[0], independent);
    batch = destination.take();
    check(batch.events.size() == 1 && batch.events[0]->error &&
          batch.events[0]->error->native == independent.error->native &&
          batch.events[0]->error->causes().size() == 1, "independent read failure is not overwritten");

    osal::IoCompletion eof;
    eof.status = osal::IoStatus::EndOfStream;
    capture->readers[1].closing = true;
    impl.complete_reader(capture->readers[1], eof);
    batch = destination.take();
    check(batch.events.size() == 1 && batch.events[0]->kind == EventKind::End,
          "confirmed EOF still wins concurrent drain deadline");
    impl.complete_reader(independent_capture->readers[1], eof);
    impl.advance_captures(std::chrono::steady_clock::now());
    check(bool(hub->wait_until(std::chrono::steady_clock::now() + test_timeout)), "failed streams still drain");
    check(hub->service_error().has_value(), "drain does not erase infrastructure diagnostic");
    inject_monitor({});
}

void check_factory_failures() {
    for (const auto point : {test::Point::CreateService, test::Point::ServiceCreated}) {
        fail_at = point;
        auto failed = OutputHub::create();
        check(!failed && failed.error().is(ErrorCode::ResourceExhausted) &&
              failed.error().is_operation(ErrorOperation::Create), "factory translates dependency allocation failure");
        auto healthy = OutputHub::create();
        check(bool(healthy), "factory can be called again after rollback");
    }
}
}

int main() {
    try {
        check_gap_and_rename();
        check_service_diagnostics();
        check_failed_completion();
        check_factory_failures();
        std::cout << "OutputHub fault tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

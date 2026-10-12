#include <vinc/time.hpp>
#include <vinc/output_hub/output_hub.hpp>
#include <vinc/output_hub/text_renderer.hpp>
#include <iostream>
#include <array>
#include <stdexcept>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;

namespace {
constexpr auto operation_timeout = 5s;
constexpr auto simulated_drain_timeout = 40ms;
constexpr auto after_display_deadline = 101ms;
constexpr int failed_spawn_attempts = 5;
constexpr std::size_t expected_large_stream_bytes = 1024 * 1024 + 17;
constexpr std::size_t large_queue_bytes = 3 * 1024 * 1024;
constexpr std::size_t large_queue_events = 1024;
constexpr std::size_t slow_queue_bytes = 4096;
constexpr std::size_t slow_queue_events = 2;
constexpr std::size_t fixture_total_queue_bytes = 4 * 1024 * 1024;
constexpr std::size_t fixture_total_queue_events = 2048;
constexpr std::size_t long_line_bytes = 10000;
constexpr std::size_t minimum_segmented_output_bytes = 8000;
constexpr auto helper_hold_time = 30000ms;
} // namespace

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(vinc::Result<T> value) {
    if (!value) {
        throw std::runtime_error(vinc::error_to_string(value.error()));
    }

    return std::move(*value);
}

vinc::Deadline soon() {
    return std::chrono::steady_clock::now() + operation_timeout;
}

struct Child {
    osal::Process process;

    ~Child() {
        (void)process.terminate();
        (void)process.wait_until(soon());
    }
};

// These expected byte strings are independent of the decoder implementation.
void check_utf8_boundaries() {
    const auto source =
        std::make_shared<output_hub::Source>(output_hub::Source{1, 1, "utf8", output_hub::Stream::Stdout});
    const auto now = std::chrono::steady_clock::now();
    const auto event = [&](std::string bytes, output_hub::EventKind kind = output_hub::EventKind::Data) {
        return std::make_shared<const output_hub::Event>(
            output_hub::Event{source, 1, {}, kind, std::move(bytes), {}});
    };
    const auto check_line = [&](std::string bytes, const std::string& expected) {
        output_hub::TextRenderer renderer;
        const auto actual = renderer.render({{event(std::move(bytes))}, {}}, now);
        check(actual == "[utf8#1 stdout] " + expected + '\n', "UTF-8 boundary rendering changed");
    };

    check_line("\xC2\xA0\xE0\xA0\x80\xF0\x90\x80\x80\xF4\x8F\xBF\xBF\n",
               "\xC2\xA0\xE0\xA0\x80\xF0\x90\x80\x80\xF4\x8F\xBF\xBF");
    check_line("\xC0\xAF\n", "\\xC0\\xAF");
    check_line("\xE0\x80\x80\n", "\\xE0\\x80\\x80");
    check_line("\xED\xA0\x80\n", "\\xED\\xA0\\x80");
    check_line("\xF4\x90\x80\x80\n", "\\xF4\\x90\\x80\\x80");
    check_line("\xC2\x9B\n", "\\xC2\\x9B");
    check_line("\xE2(\xA1\n", "\\xE2(\\xA1");

    output_hub::TextRenderer fragmented;
    check(fragmented.render({{event("\xF0\x9F")}, {}}, now).empty(), "four-byte prefix emitted early");
    check(fragmented.render({{event("\x98\x80\n")}, {}}, now) == "[utf8#1 stdout] \xF0\x9F\x98\x80\n",
          "four-byte fragments were not joined");

    output_hub::TextRenderer ended;
    check(ended.render({{event("\xE2\x82")}, {}}, now).empty(), "incomplete suffix emitted early");
    check(ended.render({{event({}, output_hub::EventKind::End)}, {}}, now) == "[utf8#1 stdout] \\xE2\\x82\n",
          "EOF lost incomplete UTF-8 suffix");
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "helper argument");
        auto processes = take(osal::ProcessService::create());
        osal::SpawnOptions options;
        options.executable = std::filesystem::absolute(argv[1]);
        // Fanout, loss accounting, mute and generation changes share one hub.
        {
            output_hub::Limits limits;
            limits.total_queue_bytes = fixture_total_queue_bytes;
            limits.total_queue_events = fixture_total_queue_events;
            auto hub_owner = take(output_hub::OutputHub::create(limits));
            auto& hub = *hub_owner;
            auto fast = take(hub.subscribe({large_queue_bytes, large_queue_events}));
            auto slow = take(hub.subscribe({slow_queue_bytes, slow_queue_events}));
            auto muted = take(hub.subscribe());
            muted.set_enabled(false);
            options.arguments = {"stdio-large"};
            auto launched = take(hub.spawn(processes, options, "source", 7));
            Child child{std::move(launched.process)};
            check(take(child.process.wait_until(soon())).value == 0, "slow consumer blocked child");
            hub.exited(launched.capture);
            check(bool(hub.wait_until(soon())), "hub did not finish");
            auto received = fast.take(large_queue_events);
            check(received.skipped.events == 0, "large queue lost data");
            std::array<std::size_t, 2> sizes{};
            std::uint64_t previous_sequence = 0;
            int end_events = 0;
            for (const auto& event : received.events) {
                check(event->sequence > previous_sequence, "sequence not increasing");
                previous_sequence = event->sequence;
                check(event->source->name == "source" && event->source->generation == 7, "source identity");
                sizes[event->source->stream == output_hub::Stream::Stdout ? 0 : 1] += event->bytes.size();
                end_events += event->kind == output_hub::EventKind::End;
            }
            check(sizes[0] == expected_large_stream_bytes && sizes[1] == sizes[0] && end_events == 2,
                  "raw fanout lost bytes/EOF");
            auto skipped = slow.take();
            check(!skipped.sources_skipped.empty(), "overflow attributes loss to its source without allocating on producer");
            check(skipped.skipped.bytes > 0 && skipped.skipped.events > 0,
                  "slow destination loss not counted");
            check(muted.take().events.empty(), "muted queue received data");
            check(hub.stats().received_bytes == sizes[0] + sizes[1], "hub failed to drain muted destination");
            muted.set_enabled(true);
            options.arguments = {"stdio-small"};
            auto restarted = take(hub.spawn(processes, options, "source", 8));
            Child next{std::move(restarted.process)};
            take(next.process.wait_until(soon()));
            hub.exited(restarted.capture);
            check(bool(hub.wait_until(soon())), "restart output drain");
            auto again = fast.take(large_queue_events);
            check(!again.events.empty(), "restart output missing");
            check(again.events.front()->source->id != received.events.front()->source->id &&
                      again.events.front()->source->generation == 8,
                  "restart reused source identity");
            check(!muted.take().events.empty(), "reenabled destination empty");
        }
        // Admission rollback, source rename and bounded tail drain.
        {
            output_hub::Limits limits;
            limits.max_captures = 1;
            limits.max_subscribers = 1;
            limits.drain_timeout = simulated_drain_timeout;
            auto hub_owner = take(output_hub::OutputHub::create(limits));
            auto& hub = *hub_owner;
            auto destination = take(hub.subscribe());
            check(!hub.subscribe(), "subscriber capacity ignored");
            destination.close();
            destination = take(hub.subscribe());
            auto missing = options;
            missing.executable += ".missing";
            for (int i = 0; i < failed_spawn_attempts; ++i) {
                const auto failed = hub.spawn(processes, missing, "failed", i);
                check(!failed && failed.error().is(output_hub::ErrorCode::SpawnFailed),
                      "spawn failure must belong to OutputHub");
                check(failed.error().native && !failed.error().is(osal::ErrorCode::NotFound),
                      "translation must retain native diagnostics without leaking OSAL classification");
                const auto causes = failed.error().causes();
                check(causes.size() == 1 && causes.front().domain == ErrorDomain::Osal &&
                          causes.front().native == failed.error().native && !failed.error().truncated(),
                      "spawn failure must retain its owned OSAL cause record");
                check(vinc::error_to_string(failed.error()).find(" <- OSAL") != std::string::npos,
                      "the common formatter must resolve both OutputHub and OSAL catalogs");
            }
            options.arguments = {"sleep", std::to_string(helper_hold_time.count())};
            auto holding = take(hub.spawn(processes, options, "holder", 1));
            Child child{std::move(holding.process)};
            check(!hub.spawn(processes, options, "extra", 2), "capture limit ignored");
            hub.set_name(holding.capture, "approved");
            auto renamed = destination.take();
            check(renamed.events.size() == 2 && renamed.events[0]->kind == output_hub::EventKind::Renamed &&
                      renamed.events[0]->source->name == "holder",
                  "rename boundary missing or previous source mutated");
            // Simulate observed parent exit while another process still holds
            // the writer. This fixture is explicitly terminated by Child RAII.
            hub.exited(holding.capture);
            check(bool(hub.wait_until(soon())), "tail drain deadline unbounded");
            check(hub.stats().truncated_streams == 2 && hub.stats().active_captures == 0,
                  "truncation not retained");
            auto batch = destination.take();
            check(batch.events.size() == 2 && batch.events[0]->kind == output_hub::EventKind::Truncated,
                  "truncation event missing");
            check(batch.events[0]->source->name == "approved" &&
                      batch.events[0]->source->id == renamed.events[0]->source->id &&
                      renamed.events[0]->source->name == "holder",
                  "rename changed identity or published source metadata");
        }
        // Child output must drain even without any subscribers.
        {
            auto hub_owner = take(output_hub::OutputHub::create());
            auto& hub = *hub_owner;
            options.arguments = {"stdio-large"};
            auto launched = take(hub.spawn(processes, options, "unobserved", 9));
            Child child{std::move(launched.process)};
            check(take(child.process.wait_until(soon())).value == 0, "no subscriber blocked capture");
            hub.exited(launched.capture);
            check(bool(hub.wait_until(soon())), "unobserved drain");
            check(take(hub.subscribe()).take().events.empty(), "unexpected output history");
        }
        // Rendering preserves UTF-8 fragments and makes terminal controls visible.
        {
            output_hub::TextRenderer renderer;
            auto source = std::make_shared<output_hub::Source>(
                output_hub::Source{1, 3, "camera", output_hub::Stream::Stdout});
            auto event = [&](std::string bytes, output_hub::EventKind kind = output_hub::EventKind::Data) {
                return std::make_shared<const output_hub::Event>(
                    output_hub::Event{source, 1, {}, kind, std::move(bytes), {}});
            };
            const auto now = std::chrono::steady_clock::now();
            check(renderer.render({{event("\xE4\xB8")}, {}}, now).empty(), "partial UTF-8 emitted early");
            auto text = renderer.render({{event("\xAD\x1b[2J\r\n")}, {}}, now);
            check(text.find("中\\x1B[2J") != std::string::npos && text.find('\x1b') == std::string::npos,
                  "UTF-8 or ANSI handling");
            check(renderer.render({{event("no newline")}, {}}, now).empty(), "line flushed too early");
            check(renderer.flush(now + after_display_deadline).find("no newline") != std::string::npos,
                  "partial line deadline ignored");
            renderer.render({{event("old")}, {}}, now);
            text = renderer.render({{event("new\n")}, {9, 1}}, now);
            check(text.find("skipped 9 bytes") != std::string::npos &&
                      text.find("oldnew") == std::string::npos,
                  "gap joined partial text");
            text = renderer.render({{event(std::string(long_line_bytes, 'a'))}, {}}, now);
            check(text.size() > minimum_segmented_output_bytes, "long line not segmented");
            text = renderer.render({{event({}, output_hub::EventKind::Truncated)}, {}}, now);
            check(text.find("not fully drained") != std::string::npos, "truncation not rendered");
            renderer.render({{event("before rename")}, {}}, now);
            text = renderer.render({{event({}, output_hub::EventKind::Renamed)}, {}}, now);
            check(text.find("camera") != std::string::npos && text.find("before rename") != std::string::npos,
                  "rename failed to flush old label fragment");
            source = std::make_shared<output_hub::Source>(
                output_hub::Source{1, 3, "approved", output_hub::Stream::Stdout});
            text = renderer.render({{event("after rename\n")}, {}}, now);
            check(text.find("approved") != std::string::npos &&
                      text.find("before rename") == std::string::npos,
                  "renderer reused old name after rename");
        }
        check_utf8_boundaries();

        std::cout << "PASS: fanout, bounded loss, mute, generations, rollback, tail deadline, plain UTF-8 "
                     "rendering\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

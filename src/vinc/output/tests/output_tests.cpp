#include <vinc/output/hub.hpp>
#include <vinc/output/text_renderer.hpp>
#include <iostream>
#include <array>
#include <stdexcept>
#include <thread>

using namespace vinc;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T take(osal::Result<T> value) {
    if (!value) throw std::runtime_error("OSAL error " + std::to_string(static_cast<int>(value.error().code)));
    return std::move(*value);
}
osal::Deadline soon() { return std::chrono::steady_clock::now() + 5s; }
struct Child {
    osal::Process process;
    ~Child() { (void)process.terminate(); (void)process.wait_until(soon()); }
};
int main(int argc, char** argv) {
    try {
        check(argc == 2, "helper argument");
        auto processes = take(osal::ProcessService::create());
        osal::SpawnOptions options; options.executable = std::filesystem::absolute(argv[1]);
        {
            output::Limits limits; limits.total_queue_bytes = 4 * 1024 * 1024; limits.total_queue_events = 2048;
            output::OutputHub hub(limits);
            auto fast = take(hub.subscribe({3 * 1024 * 1024, 1024}));
            auto slow = take(hub.subscribe({4096, 2}));
            auto muted = take(hub.subscribe()); muted.set_enabled(false);
            options.arguments = {"stdio-large"};
            auto launched = take(hub.spawn(processes, options, "source", 7));
            Child child{std::move(launched.process)};
            check(take(child.process.wait_until(soon())).value == 0, "slow consumer blocked child");
            hub.exited(launched.capture); check(bool(hub.wait_until(soon())), "hub did not finish");
            auto received = fast.take(1024);
            check(received.skipped.events == 0, "large queue lost data");
            std::array<std::size_t, 2> sizes{};
            std::uint64_t previous = 0; int ends = 0;
            for (const auto& event : received.events) {
                check(event->sequence > previous, "sequence not increasing"); previous = event->sequence;
                check(event->source->name == "source" && event->source->generation == 7, "source identity");
                sizes[event->source->stream == output::Stream::Stdout ? 0 : 1] += event->bytes.size();
                ends += event->kind == output::EventKind::End;
            }
            check(sizes[0] == 1024 * 1024 + 17 && sizes[1] == sizes[0] && ends == 2, "raw fanout lost bytes/EOF");
            auto skipped = slow.take();
            check(skipped.skipped.bytes > 0 && skipped.skipped.events > 0, "slow destination loss not counted");
            check(muted.take().events.empty(), "muted queue received data");
            check(hub.stats().received_bytes == sizes[0] + sizes[1], "hub failed to drain muted destination");
            muted.set_enabled(true);
            options.arguments = {"stdio-small"};
            auto restarted = take(hub.spawn(processes, options, "source", 8));
            Child next{std::move(restarted.process)};
            take(next.process.wait_until(soon())); hub.exited(restarted.capture);
            check(bool(hub.wait_until(soon())), "restart output drain");
            auto again = fast.take(1024);
            check(!again.events.empty(), "restart output missing");
            check(again.events.front()->source->id != received.events.front()->source->id &&
                again.events.front()->source->generation == 8, "restart reused source identity");
            check(!muted.take().events.empty(), "reenabled destination empty");
        }
        {
            output::Limits limits; limits.max_captures = 1; limits.max_subscribers = 1; limits.drain_timeout = 40ms;
            output::OutputHub hub(limits);
            auto destination = take(hub.subscribe());
            check(!hub.subscribe(), "subscriber capacity ignored");
            destination.close(); destination = take(hub.subscribe());
            auto missing = options; missing.executable += ".missing";
            for (int i = 0; i < 5; ++i) check(!hub.spawn(processes, missing, "failed", i), "missing helper started");
            options.arguments = {"sleep", "30000"};
            auto holding = take(hub.spawn(processes, options, "holder", 1));
            Child child{std::move(holding.process)};
            check(!hub.spawn(processes, options, "extra", 2), "capture limit ignored");
            hub.set_name(holding.capture, "approved");
            auto renamed = destination.take();
            check(renamed.events.size() == 2 && renamed.events[0]->kind == output::EventKind::Renamed &&
                renamed.events[0]->source->name == "holder", "rename boundary missing or previous source mutated");
            // Simulate observed parent exit while another process still holds
            // the writer. This fixture is explicitly terminated by Child RAII.
            hub.exited(holding.capture);
            check(bool(hub.wait_until(soon())), "tail drain deadline unbounded");
            check(hub.stats().truncated_streams == 2 && hub.stats().active_captures == 0, "truncation not retained");
            auto batch = destination.take();
            check(batch.events.size() == 2 && batch.events[0]->kind == output::EventKind::Truncated, "truncation event missing");
            check(batch.events[0]->source->name == "approved" &&
                batch.events[0]->source->id == renamed.events[0]->source->id &&
                renamed.events[0]->source->name == "holder", "rename changed identity or published source metadata");
        }
        {
            output::OutputHub hub;
            options.arguments = {"stdio-large"};
            auto launched = take(hub.spawn(processes, options, "unobserved", 9));
            Child child{std::move(launched.process)};
            check(take(child.process.wait_until(soon())).value == 0, "no subscriber blocked capture");
            hub.exited(launched.capture); check(bool(hub.wait_until(soon())), "unobserved drain");
            check(take(hub.subscribe()).take().events.empty(), "unexpected output history");
        }
        {
            output::TextRenderer renderer;
            auto source = std::make_shared<output::Source>(output::Source{1, 3, "camera", output::Stream::Stdout});
            auto event = [&](std::string bytes, output::EventKind kind = output::EventKind::Data) {
                return std::make_shared<const output::Event>(output::Event{source, 1, {}, kind, std::move(bytes), {}});
            };
            const auto now = std::chrono::steady_clock::now();
            check(renderer.render({{event("\xE4\xB8")}, {}}, now).empty(), "partial UTF-8 emitted early");
            auto text = renderer.render({{event("\xAD\x1b[2J\r\n")}, {}}, now);
            check(text.find("中\\x1B[2J") != std::string::npos && text.find('\x1b') == std::string::npos, "UTF-8 or ANSI handling");
            check(renderer.render({{event("no newline")}, {}}, now).empty(), "line flushed too early");
            check(renderer.flush(now + 101ms).find("no newline") != std::string::npos, "partial line deadline ignored");
            renderer.render({{event("old")}, {}}, now);
            text = renderer.render({{event("new\n")}, {9, 1}}, now);
            check(text.find("skipped 9 bytes") != std::string::npos && text.find("oldnew") == std::string::npos, "gap joined partial text");
            text = renderer.render({{event(std::string(10000, 'a'))}, {}}, now);
            check(text.size() > 8000, "long line not segmented");
            text = renderer.render({{event({}, output::EventKind::Truncated)}, {}}, now);
            check(text.find("not fully drained") != std::string::npos, "truncation not rendered");
            renderer.render({{event("before rename")}, {}}, now);
            text = renderer.render({{event({}, output::EventKind::Renamed)}, {}}, now);
            check(text.find("camera") != std::string::npos && text.find("before rename") != std::string::npos, "rename failed to flush old label fragment");
            source = std::make_shared<output::Source>(output::Source{1, 3, "approved", output::Stream::Stdout});
            text = renderer.render({{event("after rename\n")}, {}}, now);
            check(text.find("approved") != std::string::npos && text.find("before rename") == std::string::npos, "renderer reused old name after rename");
        }
        std::cout << "PASS: fanout, bounded loss, mute, generations, rollback, tail deadline, plain UTF-8 rendering\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

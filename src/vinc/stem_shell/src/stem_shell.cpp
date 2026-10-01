// Shell event loop: keep keyboard/control input responsive while command jobs run.
// A foreground script suppresses ordinary input but keeps cancel and full shutdown available.

#include <stem_shell/stem_shell.hpp>
#include <stem_shell/command_router.hpp>
#include <stem_shell/tcl_runtime.hpp>
#include <vinc/osal/terminal.hpp>
#include <atomic>
#include <future>
#include <iostream>
#include <thread>
#include <variant>

namespace vinc {
using namespace std::chrono_literals;
struct StemShell::Impl {
    ModuleManager& manager;
    std::ostream& output;
    shell::CommandRouter router;
    shell::LineEditor editor;
    std::atomic<bool> closing{false};
    std::string last_error;
    using Reply = std::variant<shell::CommandResult, shell::CompletionResult>;
    struct Job { std::string target; std::future<Reply> reply; };
    std::vector<Job> jobs;
    osal::Terminal* terminal = nullptr;
    bool prompt = false, shutdown_complete = false, rejected_line = false;
    control::Deadline next_report{};
    // Destroy/join the script before any state captured by its dispatch callback.
    shell::TclRuntime script;
    Impl(ModuleManager& m, std::ostream& out) : manager(m), output(out), router(m) {
        for (const auto& name : shell::tcl_reserved_commands()) {
            auto result = manager.reserve_command_name(name);
            if (!result) throw std::runtime_error(shell::describe(result.error()));
        }
        jobs.reserve(16);
    }
    bool interactive() const { return terminal && terminal->interactive() && terminal->ansi_output(); }
    void clear_prompt() {
        if (prompt && interactive()) output << "\r\x1b[2K";
        prompt = false;
    }
    void redraw() {
        if (closing.load() || script.active() || !interactive()) return;
        const auto columns = terminal->columns();
        output << "\r\x1b[2KVINC > " << editor.visible_tail(columns > 8 ? columns - 8 : 0) << std::flush;
        prompt = true;
    }
    void show(const std::string& message) {
        if (message.empty()) return;
        clear_prompt(); output << message << '\n' << std::flush; redraw();
    }
    void shutdown() {
        if (closing.exchange(true)) return;
        script.cancel();
        manager.begin_shutdown();
        clear_prompt(); output << "Shutting down: waiting for all module processes...\n" << std::flush;
    }
    shell::CommandResult execute(const std::vector<std::string>& args, std::stop_token stop = {}) {
        try { return router.execute(args, stop); }
        catch (const std::exception& e) { return {false, false, std::string("Command error: ") + e.what()}; }
        catch (...) { return {false, false, "Command threw an unknown exception"}; }
    }
    // Bound concurrent Shell jobs and reject a second job for the same target.
    // ModuleManager separately guards requests already sent to the child process.
    bool available(const std::string& target) {
        if (jobs.size() >= 16) { show("Busy: 16 requests are already in flight"); return false; }
        for (const auto& job : jobs) if (job.target == target) { show("Busy: " + target + " already has a request in flight"); return false; }
        return true;
    }
    void start_script(std::filesystem::path path, std::vector<std::string> args = {}) {
        if (script.active()) { show("Busy: a script is already running"); return; }
        clear_prompt();
        try {
            script.start(std::move(path), std::move(args), [this](const auto& command, std::stop_token stop) {
                if (closing.load()) return shell::CommandResult{false, false, "Framework is shutting down"};
                return execute(command, stop);
            });
            show("Script running. Ctrl+D cancels the script; Ctrl+C shuts down the framework.");
        } catch (const std::exception& e) { show(std::string("Cannot start script: ") + e.what()); }
    }
    void cancel_script() {
        show(script.cancel() ? "Script cancellation requested" : "No script is running");
    }
    void submit(std::string input, bool completion = false) {
        if (closing.load()) return;
        auto parsed = shell::parse_line(input, completion);
        if (!parsed) { show("Parse error: " + parsed.error()); return; }
        const auto target = parsed->arguments.empty() ? std::string{} : parsed->arguments.front();
        if (!completion && parsed->arguments.empty()) return;
        if (!completion && target == "srun") {
            if (parsed->arguments.size() < 2) { show("Usage: srun file.tcl [args...]"); return; }
            const auto& file = parsed->arguments[1];
            start_script(std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(file.data()), file.size())),
                {parsed->arguments.begin() + 2, parsed->arguments.end()}); return;
        }
        if (!completion && target == "scancel") {
            if (parsed->arguments.size() != 1) show("scancel takes no arguments");
            else cancel_script();
            return;
        }
        if (!completion && router.builtin(target)) {
            auto result = execute(parsed->arguments);
            if (result.shutdown) shutdown(); else show(result.message);
            return;
        }
        if (!available(target)) return;
        try {
            jobs.push_back({target, std::async(std::launch::async, [this, input = std::move(input), args = std::move(parsed->arguments), completion]() -> Reply {
                if (!completion) return execute(args);
                try { return router.complete(input); }
                catch (const std::exception& e) { return shell::CompletionResult{input, 0, {}, e.what()}; }
                catch (...) { return shell::CompletionResult{input, 0, {}, "Completion failed"}; }
            })});
        } catch (const std::exception& e) { show(std::string("Cannot start command: ") + e.what()); }
    }
    // Consume worker results only on the Shell thread, keeping editor state and output
    // out of command callbacks. Script errors are displayed without shutting down modules.
    void collect() {
        const auto script_output = script.take_output();
        if (!script_output.empty()) { clear_prompt(); output << script_output << std::flush; redraw(); }
        if (script.ready()) {
            auto result = script.finish();
            const auto tail = script.take_output();
            if (!tail.empty()) output << tail << std::flush;
            if (result.shutdown) shutdown();
            else { show(result.message); redraw(); }
        }
        for (auto it = jobs.begin(); it != jobs.end();) {
            if (it->reply.wait_for(0ms) != std::future_status::ready) { ++it; continue; }
            auto reply = it->reply.get(); it = jobs.erase(it);
            if (const auto* command = std::get_if<shell::CommandResult>(&reply)) {
                if (command->shutdown) shutdown(); else show(command->message);
                continue;
            }
            auto& completion = std::get<shell::CompletionResult>(reply);
            // A late Tab response must not overwrite subsequently edited text.
            if (closing.load() || editor.line() != completion.input) continue;
            if (!completion.error.empty()) { show(completion.error); continue; }
            if (completion.choices.size() == 1) {
                if (!editor.replace(editor.line().substr(0, completion.replace_start) + shell::quote_argument(completion.choices.front())))
                    show("Completion exceeds command length limit");
                redraw();
            } else if (!completion.choices.empty()) {
                std::string text;
                for (const auto& choice : completion.choices) text += shell::quote_argument(choice) + "  ";
                show(text);
            }
        }
    }
    // Shutdown is a wait for observed completion, not just a batch of terminate requests.
    // Keep collecting jobs while supervision and Bus cleanup finish; repeated Ctrl+C does not skip it.
    void wait_shutdown() {
        if (shutdown_complete) return;
        shutdown();
        for (;;) {
            const auto result = manager.wait_shutdown(control::Clock::now() + 250ms);
            collect();
            if (result) break;
            const auto now = control::Clock::now();
            if (now >= next_report) {
                output << "Still waiting:";
                for (const auto& s : manager.snapshots()) {
                    if (s.state == ModuleState::Stopped || s.state == ModuleState::Failed) continue;
                    output << " " << s.name << "(pid=" << s.pid << ")";
                    if (s.os_error) output << " [" << s.last_error << ": " << s.os_error->native.message() << "]";
                }
                output << " " << result.error().message << '\n' << std::flush;
                next_report = now + 1s;
            }
        }
        while (!jobs.empty() || script.active()) { collect(); if (!jobs.empty() || script.active()) std::this_thread::sleep_for(10ms); }
        shutdown_complete = true;
        output << "All module processes exited; Bus cleanup complete.\n" << std::flush;
    }
};
StemShell::StemShell() : StemShell(ModuleManager::get(), std::cout) {}
StemShell::StemShell(ModuleManager& manager, std::ostream& out) : impl_(std::make_unique<Impl>(manager, out)) {}
StemShell::~StemShell() = default;
std::string StemShell::get_last_error() const { return impl_->last_error; }
void StemShell::request_shutdown() { impl_->shutdown(); }
void StemShell::wait_shutdown() { impl_->wait_shutdown(); }
int8_t StemShell::process_line(const std::string& line) {
    impl_->last_error.clear();
    if (impl_->closing.load()) { impl_->last_error = "framework is shutting down"; return -1; }
    auto parsed = shell::parse_line(line);
    if (!parsed) { impl_->last_error = parsed.error(); return -1; }
    auto result = impl_->execute(parsed->arguments);
    if (!result.success) { impl_->last_error = result.message; return -1; }
    if (result.shutdown) impl_->shutdown(); else impl_->show(result.message);
    return 0;
}
void StemShell::run(std::filesystem::path startup_script, std::vector<std::string> script_args) {
    if (impl_->closing.load()) { impl_->wait_shutdown(); return; }
    auto opened = osal::Terminal::open();
    if (!opened) {
        impl_->show("Cannot open terminal: " + opened.error().native.message());
        impl_->wait_shutdown(); return;
    }
    auto terminal = std::move(*opened); impl_->terminal = terminal.get();
    struct Detach { Impl& state; ~Detach() { state.terminal = nullptr; } } detach{*impl_};
    // A startup deployment can intentionally have no terminal. Its stdin EOF must not
    // turn a finished or failed script into an automatic framework shutdown.
    const bool headless_deployment = !startup_script.empty() && !terminal->interactive();
    if (!startup_script.empty()) impl_->start_script(std::move(startup_script), std::move(script_args));
    impl_->show("VINC ready. Type help; exit or Ctrl+C shuts down all modules."); impl_->redraw();
    try {
        while (!impl_->closing.load()) {
            impl_->collect();
            auto event = terminal->read_until(control::Clock::now() + 20ms);
            if (!event) { impl_->show("Input error: " + event.error().native.message()); impl_->shutdown(); break; }
            switch (event->key) {
            case osal::TerminalKey::Interrupt: impl_->shutdown(); break;
            case osal::TerminalKey::Cancel: impl_->cancel_script(); break;
            case osal::TerminalKey::End:
                if (headless_deployment) std::this_thread::sleep_for(20ms);
                else impl_->shutdown();
                break;
            case osal::TerminalKey::Text:
                if (impl_->script.active()) break;
                if (!impl_->editor.append(event->text) && !impl_->rejected_line) {
                    impl_->rejected_line = true;
                    impl_->show("Command exceeds 4096 bytes; this line will not execute");
                }
                impl_->redraw(); break;
            case osal::TerminalKey::Backspace: if (!impl_->script.active()) { impl_->editor.backspace(); impl_->redraw(); } break;
            case osal::TerminalKey::Up: if (!impl_->script.active()) { impl_->editor.older(); impl_->redraw(); } break;
            case osal::TerminalKey::Down: if (!impl_->script.active()) { impl_->editor.newer(); impl_->redraw(); } break;
            case osal::TerminalKey::Tab: if (!impl_->script.active()) impl_->submit(impl_->editor.line(), true); break;
            case osal::TerminalKey::Enter: {
                if (impl_->script.active()) break;
                if (impl_->rejected_line) impl_->editor.replace({}); // Do not retain an executable truncated prefix in history.
                auto line = impl_->editor.submit(); impl_->clear_prompt();
                if (terminal->interactive()) impl_->output << "VINC > " << line << '\n';
                if (impl_->rejected_line) { impl_->rejected_line = false; impl_->show("Rejected oversized command"); }
                else impl_->submit(std::move(line));
                impl_->redraw(); break;
            }
            default: break;
            }
        }
    } catch (const std::exception& e) { impl_->show(std::string("Shell error: ") + e.what()); }
    impl_->wait_shutdown();
}
}

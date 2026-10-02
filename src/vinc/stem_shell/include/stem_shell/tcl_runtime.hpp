#pragma once

// One foreground script job, with a fresh Tcl interpreter created on its worker thread.
// The Shell owns control calls and drains output; the worker calls the supplied command dispatcher.

#include <stem_shell/command_router.hpp>
#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>

namespace vinc::shell {
// Call once from main, before constructing Shells or creating Tcl workers.
void initialize_tcl(const char* executable);
std::vector<std::string> tcl_reserved_commands();
struct ScriptResult { bool success = false; bool canceled = false; bool shutdown = false; std::string message; };
class TclRuntime {
public:
    using Dispatch = std::function<CommandResult(const std::vector<std::string>&, std::stop_token)>;
    TclRuntime();
    ~TclRuntime();
    TclRuntime(const TclRuntime&) = delete;
    TclRuntime& operator=(const TclRuntime&) = delete;
    // Returns false while a previous job is still owned, even if it has completed.
    // Call finish() to collect that result before starting another script.
    bool start(std::filesystem::path, std::vector<std::string>, Dispatch);
    // Request cooperative Tcl unwind. Already-issued module commands are not undone;
    // blocking native code can delay cancellation until control returns to Tcl.
    bool cancel();
    bool active() const;
    bool ready() const;
    // Collect the active job (may block); the event loop normally checks ready() first.
    ScriptResult finish();
    // Drain the bounded output buffer; excess output is reported as truncated.
    std::string take_output();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

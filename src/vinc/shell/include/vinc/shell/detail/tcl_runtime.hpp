#pragma once

// One foreground script job, with a fresh Tcl interpreter created on its worker thread.
// The Shell owns control calls and drains output; the worker calls the supplied command dispatcher.

#include <vinc/shell/detail/command_router.hpp>
#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>

namespace vinc::shell {
// Call from main before creating Shells/workers. A failed initialization may be
// retried after deploying the missing resources; success is idempotent.
Result<void> initialize_tcl(const char* executable);
Result<std::filesystem::path> executable_directory();
Result<std::vector<std::string>> tcl_reserved_commands();

struct ScriptCompletion {
    bool shutdown = false;
};

// Text is bounded presentation context, never an alternate error identity.
struct ScriptDiagnostic {
    std::string file;
    std::int32_t line = 0;
    std::string details;
};

struct ScriptReport {
    Result<ScriptCompletion> outcome;
    ScriptDiagnostic diagnostic;
};

std::string script_text(const ScriptReport& report);

class TclRuntime {
public:
    using Dispatch = std::function<Result<CommandReply>(const std::vector<std::string>&, std::stop_token)>;
    TclRuntime();
    ~TclRuntime();
    TclRuntime(const TclRuntime&) = delete;
    TclRuntime& operator=(const TclRuntime&) = delete;
    // Returns Busy while a previous job is still owned, even if it has completed.
    // Call finish() to collect that result before starting another script.
    Result<void> start(std::filesystem::path path, std::vector<std::string> arguments, Dispatch dispatch);
    // Request cooperative Tcl unwind. Already-issued module commands are not undone;
    // blocking native code can delay cancellation until control returns to Tcl.
    bool cancel();
    bool active() const;
    bool ready() const;
    // Requires active() (contract violation otherwise). May block; the event loop
    // normally checks ready() first. The returned outcome owns its error chain.
    ScriptReport finish();
    // Drain the bounded output buffer; excess output is reported as truncated.
    std::string take_output();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

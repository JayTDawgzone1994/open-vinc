// Embed unmodified Tcl through public APIs. Each run owns a fresh worker/interpreter;
// commands reuse the framework router, and cancellation unwinds the current script only.

#include <stem_shell/tcl_runtime.hpp>
#include <tcl.h>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace vinc::shell {
namespace {
using namespace std::chrono_literals;
std::once_flag initialized;
std::filesystem::path library_path;
std::string utf8(const std::filesystem::path& p) {
    auto s = p.generic_u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()};
}
std::string string(Tcl_Obj* obj) {
    Tcl_Size size = 0; const char* p = Tcl_GetStringFromObj(obj, &size);
    return {p, static_cast<std::size_t>(size)};
}
int error(Tcl_Interp* interp, const std::string& text) {
    Tcl_SetObjResult(interp, Tcl_NewStringObj(text.data(), static_cast<Tcl_Size>(text.size()))); return TCL_ERROR;
}
using Interpreter = std::unique_ptr<Tcl_Interp, decltype(&Tcl_DeleteInterp)>;
Interpreter interpreter() {
    if (library_path.empty()) throw std::runtime_error("Call initialize_tcl(argv[0]) before creating a Shell");
    Interpreter interp(Tcl_CreateInterp(), Tcl_DeleteInterp);
    const auto path = utf8(library_path);
    Tcl_SetVar(interp.get(), "tcl_library", path.c_str(), TCL_GLOBAL_ONLY);
    if (Tcl_Init(interp.get()) != TCL_OK) throw std::runtime_error(Tcl_GetStringResult(interp.get()));
    return interp;
}
int original(Tcl_Interp* interp, const char* name, Tcl_Size count, Tcl_Obj* const values[]) {
    std::vector<Tcl_Obj*> args(values, values + count);
    args[0] = Tcl_NewStringObj(name, -1); Tcl_IncrRefCount(args[0]);
    const int result = Tcl_EvalObjv(interp, count, args.data(), TCL_EVAL_DIRECT);
    Tcl_DecrRefCount(args[0]); return result;
}
}
void initialize_tcl(const char* executable) {
    std::call_once(initialized, [=] {
        Tcl_FindExecutable(executable);
        const auto* name = Tcl_GetNameOfExecutable();
        if (!name || !*name) throw std::runtime_error("Cannot locate executable for Tcl runtime resources");
        library_path = std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(name))).parent_path() / "lib" / "tcl9.1";
        if (!std::filesystem::is_regular_file(library_path / "init.tcl"))
            throw std::runtime_error("Missing Tcl runtime resources: " + utf8(library_path));
    });
}
std::vector<std::string> tcl_reserved_commands() {
    auto interp = interpreter();
    if (Tcl_Eval(interp.get(), "concat [info commands] [array names auto_index]") != TCL_OK)
        throw std::runtime_error(Tcl_GetStringResult(interp.get()));
    Tcl_Size count; Tcl_Obj** values;
    if (Tcl_ListObjGetElements(interp.get(), Tcl_GetObjResult(interp.get()), &count, &values) != TCL_OK)
        throw std::runtime_error("Cannot enumerate Tcl commands");
    std::vector<std::string> result{"srun", "scancel"};
    for (Tcl_Size i = 0; i < count; ++i) result.push_back(string(values[i]));
    return result;
}
struct TclRuntime::Impl {
    std::future<ScriptResult> job;
    std::mutex mutex;
    std::condition_variable wake;
    // Tcl evaluation stays on the worker. This pointer is only published so another
    // thread can request cancellation while the same mutex prevents interpreter deletion.
    Tcl_Interp* live = nullptr; // guarded against deletion while cancel is called
    std::atomic<bool> canceled{false};
    std::atomic<bool> shutdown{false};
    std::stop_source stop;
    std::string output;
    bool truncated = false;
    Dispatch dispatch;
    // Do not let a fast puts loop grow memory without bound while the UI is busy.
    // Trim only at UTF-8 boundaries and report that some output was dropped.
    void write(std::string_view text) {
        std::lock_guard lock(mutex);
        constexpr std::size_t limit = 256 * 1024;
        auto n = (std::min)(text.size(), limit - output.size());
        while (n && n < text.size() && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
        output.append(text.substr(0, n)); truncated |= n != text.size();
    }
    bool cancel() {
        std::lock_guard lock(mutex);
        canceled.store(true); wake.notify_all();
        stop.request_stop();
        if (live) Tcl_CancelEval(live, nullptr, nullptr, TCL_CANCEL_UNWIND);
        return true;
    }
    int check(Tcl_Interp* interp) {
        if (!canceled.load()) return TCL_OK;
        Tcl_CancelEval(interp, nullptr, nullptr, TCL_CANCEL_UNWIND);
        return error(interp, "Script canceled");
    }
    // Bridge already-parsed Tcl objects directly to argument vectors. No command-line
    // reparse occurs, so spaces, empty arguments and literal metacharacters survive.
    static int command(void* data, Tcl_Interp* interp, Tcl_Size count, Tcl_Obj* const values[]) noexcept {
        auto& self = *static_cast<Impl*>(data);
        try {
            if (self.check(interp) != TCL_OK) return TCL_ERROR;
            auto name = string(values[0]);
            while (name.starts_with("::")) name.erase(0, 2);
            if (name == "exit") {
                if (count != 1) return error(interp, "exit takes no arguments; it shuts down the framework");
                self.shutdown.store(true); self.cancel(); return self.check(interp);
            }
            if (name == "scancel") {
                if (count != 1) return error(interp, "scancel takes no arguments");
                self.cancel(); return self.check(interp);
            }
            if (name == "srun") return error(interp, "Busy: a script is already running; use source to include a file");
            // Replace only the blocking sleep form with a cancellable wait. Delegate timer/event
            // forms to Tcl so its normal event-loop semantics remain available.
            if (name == "after") {
                Tcl_WideInt ms;
                if (count == 2 && Tcl_GetWideIntFromObj(nullptr, values[1], &ms) == TCL_OK) {
                    // Short waits avoid overflowing chrono for arbitrarily large Tcl values.
                    std::unique_lock lock(self.mutex);
                    while (ms > 0 && !self.canceled.load()) {
                        const auto slice = (std::min)(ms, Tcl_WideInt{20});
                        self.wake.wait_for(lock, std::chrono::milliseconds(slice), [&] { return self.canceled.load(); });
                        ms -= slice;
                    }
                    lock.unlock(); return self.check(interp);
                }
                return original(interp, "::vinc::native_after", count, values);
            }
            if (name == "puts") {
                Tcl_Size index = 1; bool newline = true;
                if (index < count && string(values[index]) == "-nonewline") { newline = false; ++index; }
                std::string channel = "stdout";
                if (count - index == 2) channel = string(values[index++]);
                if (count - index != 1 || (channel != "stdout" && channel != "stderr"))
                    return original(interp, "::vinc::native_puts", count, values);
                self.write(string(values[index])); if (newline) self.write("\n"); return TCL_OK;
            }
            // Tcl reaches module names through unknown. Keep Tcl's original unknown as a
            // fallback for autoloaded commands that the framework does not recognize.
            const Tcl_Size first = name == "unknown" ? 1 : 0;
            std::vector<std::string> args;
            for (auto i = first; i < count; ++i) args.push_back(string(values[i]));
            if (!args.empty()) while (args.front().starts_with("::")) args.front().erase(0, 2);
            auto result = self.dispatch(args, self.stop.get_token());
            if (self.check(interp) != TCL_OK) return TCL_ERROR;
            if (!result.success) {
                if (name == "unknown" && result.message.starts_with("Unknown command:"))
                    return original(interp, "::vinc::native_unknown", count, values);
                return error(interp, result.message);
            }
            if (result.shutdown) { self.shutdown.store(true); self.cancel(); return self.check(interp); }
            if (!result.message.empty()) { self.write(result.message); self.write("\n"); }
            Tcl_SetObjResult(interp, Tcl_NewIntObj(0)); return TCL_OK;
        } catch (const std::exception& e) { return error(interp, e.what()); }
        catch (...) { return error(interp, "Native script command failed"); }
    }
    // Create, evaluate and destroy this interpreter on the same worker thread.
    // No variables, procedures or timers are carried into the next script run.
    ScriptResult run(const std::filesystem::path& path, const std::vector<std::string>& args) {
        // Finalize only this worker's Tcl state, after interpreter destruction.
        struct Finalize { ~Finalize() { Tcl_FinalizeThread(); } } finalize;
        try {
            auto interp = interpreter();
            if (Tcl_Eval(interp.get(), "namespace eval ::vinc {}; rename unknown ::vinc::native_unknown; rename puts ::vinc::native_puts; rename after ::vinc::native_after") != TCL_OK)
                throw std::runtime_error(Tcl_GetStringResult(interp.get()));
            for (const auto* name : {"unknown", "puts", "after", "exit", "help", "srun", "scancel"})
                Tcl_CreateObjCommand2(interp.get(), name, command, this, nullptr);
            const auto file = utf8(path);
            Tcl_SetVar2Ex(interp.get(), "argv0", nullptr, Tcl_NewStringObj(file.data(), static_cast<Tcl_Size>(file.size())), TCL_GLOBAL_ONLY);
            auto* list = Tcl_NewListObj(0, nullptr);
            for (const auto& arg : args) Tcl_ListObjAppendElement(interp.get(), list, Tcl_NewStringObj(arg.data(), static_cast<Tcl_Size>(arg.size())));
            Tcl_SetVar2Ex(interp.get(), "argv", nullptr, list, TCL_GLOBAL_ONLY);
            Tcl_SetVar2Ex(interp.get(), "argc", nullptr, Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(args.size())), TCL_GLOBAL_ONLY);
            {
                std::lock_guard lock(mutex); live = interp.get();
            }
            // Detach before deleting the interpreter, including exception paths.
            struct Detach { Impl& self; ~Detach() { std::lock_guard lock(self.mutex); self.live = nullptr; } } detach{*this};
            int code = canceled.load() ? TCL_ERROR : Tcl_EvalFile(interp.get(), file.c_str());
            if (shutdown.load()) return {true, false, true, "Script requested framework shutdown"};
            if (canceled.load()) return {false, true, false, "Script canceled: " + file};
            if (code != TCL_OK) {
                const char* details = Tcl_GetVar(interp.get(), "errorInfo", TCL_GLOBAL_ONLY);
                return {false, false, false, "Script error in " + file + " (line " + std::to_string(Tcl_GetErrorLine(interp.get())) + "):\n" + (details ? details : Tcl_GetStringResult(interp.get()))};
            }
            return {true, false, false, "Script completed: " + file};
        } catch (const std::exception& e) { return {false, false, false, std::string("Script error: ") + e.what()}; }
    }
};
TclRuntime::TclRuntime() : impl_(std::make_unique<Impl>()) {}
TclRuntime::~TclRuntime() { if (active()) { cancel(); impl_->job.wait(); } }
bool TclRuntime::start(std::filesystem::path path, std::vector<std::string> args, Dispatch dispatch) {
    if (active()) return false;
    impl_->canceled.store(false); impl_->shutdown.store(false); impl_->stop = {}; impl_->dispatch = std::move(dispatch);
    impl_->job = std::async(std::launch::async, [this, path = std::move(path), args = std::move(args)] { return impl_->run(path, args); });
    return true;
}
bool TclRuntime::active() const { return impl_->job.valid(); }
bool TclRuntime::ready() const { return active() && impl_->job.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }
bool TclRuntime::cancel() { return active() && impl_->cancel(); }
ScriptResult TclRuntime::finish() { return impl_->job.get(); }
std::string TclRuntime::take_output() {
    std::lock_guard lock(impl_->mutex);
    auto result = std::move(impl_->output); impl_->output.clear();
    if (impl_->truncated) { result += "\n[Script output truncated]\n"; impl_->truncated = false; }
    return result;
}
}

#include <vinc/osal/path.hpp>
// Embed unmodified Tcl through public APIs. Each run owns a fresh worker/interpreter;
// commands reuse the framework router, and cancellation unwinds the current script only.

#include <vinc/shell/detail/tcl_runtime.hpp>
#include <tcl.h>
#include <vinc/contract.hpp>
#include <vinc/osal/error.hpp>
#include <algorithm>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace vinc::shell {
namespace {
using namespace std::chrono_literals;
std::mutex tcl_initialization_mutex;
std::filesystem::path library_path;

Result<std::string> tcl_path(const std::filesystem::path& path) {
    auto text = osal::path_to_utf8(path);
    if (!text) {
        return std::unexpected(text.error());
    }

#ifdef _WIN32
    // Tcl consumes forward separators. POSIX backslashes are filename bytes and must stay intact.
    std::replace(text->begin(), text->end(), '\\', '/');
#endif
    return text;
}

std::string object_text(Tcl_Obj* object) {
    Tcl_Size size = 0;
    const char* bytes = Tcl_GetStringFromObj(object, &size);
    return {bytes, static_cast<std::size_t>(size)};
}

int error(Tcl_Interp* interpreter_handle, const std::string& text) {
    Tcl_SetObjResult(interpreter_handle, Tcl_NewStringObj(text.data(), static_cast<Tcl_Size>(text.size())));
    return TCL_ERROR;
}

using Interpreter = std::unique_ptr<Tcl_Interp, decltype(&Tcl_DeleteInterp)>;

std::string bounded_diagnostic(std::string_view text) {
    constexpr auto limit = config::shell_script_result_bytes;
    constexpr std::string_view marker = "\n[Script diagnostic truncated]";
    static_assert(limit > marker.size());
    if (text.size() <= limit) {
        return std::string(text);
    }

    auto bytes = limit - marker.size();
    while (bytes && (static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80) {
        --bytes;
    }

    return std::string(text.substr(0, bytes)) + std::string(marker);
}

void capture_diagnostic(Tcl_Interp* interpreter, ScriptDiagnostic& diagnostic) {
    diagnostic.line = Tcl_GetErrorLine(interpreter);
    const char* details = Tcl_GetVar(interpreter, "errorInfo", TCL_GLOBAL_ONLY);
    const std::string_view text(details ? details : Tcl_GetStringResult(interpreter));
    diagnostic.details = bounded_diagnostic(text);
}

Result<Interpreter> create_interpreter(ScriptDiagnostic& diagnostic) {
    std::filesystem::path path;
    {
        std::lock_guard lock(tcl_initialization_mutex);
        if (library_path.empty()) {
            return std::unexpected(Error{ErrorCode::TclNotInitialized, ErrorOperation::CreateInterpreter});
        }

        path = library_path;
    }

    Interpreter interpreter_handle(Tcl_CreateInterp(), Tcl_DeleteInterp);
    const auto text = tcl_path(path);
    if (!text) {
        return std::unexpected(
            wrap_error(ErrorCode::TclInitializationFailed, ErrorOperation::CreateInterpreter, text.error()));
    }

    Tcl_SetVar(interpreter_handle.get(), "tcl_library", text->c_str(), TCL_GLOBAL_ONLY);
    if (Tcl_Init(interpreter_handle.get()) != TCL_OK) {
        capture_diagnostic(interpreter_handle.get(), diagnostic);
        return std::unexpected(Error{ErrorCode::TclInitializationFailed, ErrorOperation::CreateInterpreter});
    }

    return interpreter_handle;
}

// Tcl objects are reference counted. These owners retain newly created temporary
// lists/options until the interpreter or parent list takes its own reference.
struct ObjectOwner {
    Tcl_Obj* value;

    explicit ObjectOwner(Tcl_Obj* object) : value(object) {
        Tcl_IncrRefCount(value);
    }

    ~ObjectOwner() {
        Tcl_DecrRefCount(value);
    }

    ObjectOwner(const ObjectOwner&) = delete;
    ObjectOwner& operator=(const ObjectOwner&) = delete;
};

// Carry the error with Tcl return options, not in a mutable "last error" slot.
// catch may consume it; return -options preserves it on an explicit rethrow.
int framework_error(Tcl_Interp* interpreter, const Error& failure, std::string_view context = {}) {
    error(interpreter, vinc::error_to_string(failure));
    if (!context.empty()) {
        const auto text = bounded_diagnostic(context);
        Tcl_AppendResult(interpreter, "\n", text.c_str(), nullptr);
    }

    ObjectOwner list(Tcl_NewListObj(0, nullptr));
    Tcl_ListObjAppendElement(interpreter, list.value, Tcl_NewStringObj("VINC", -1));
    auto append = [&](Tcl_WideInt value) {
        Tcl_ListObjAppendElement(interpreter, list.value, Tcl_NewWideIntObj(value));
    };
    append(1); // In-process Tcl errorCode format version.
    append(static_cast<Tcl_WideInt>(failure.causes().size() + 1));
    append(failure.truncated() ? 1 : 0);
    auto record = [&](const ErrorRecord& value) {
        append(static_cast<std::uint32_t>(value.domain));
        append(value.code);
        append(value.operation);
        append(static_cast<std::uint32_t>(value.native.source));
        append(value.native.code);
    };
    record(failure.record());
    for (const auto& cause : failure.causes()) {
        record(cause);
    }

    Tcl_SetObjErrorCode(interpreter, list.value);
    return TCL_ERROR;
}

std::optional<Error> framework_cause(Tcl_Interp* interpreter, int return_code) {
    ObjectOwner options(Tcl_GetReturnOptions(interpreter, return_code));
    ObjectOwner key(Tcl_NewStringObj("-errorcode", -1));
    Tcl_Obj* list = nullptr;
    if (Tcl_DictObjGet(nullptr, options.value, key.value, &list) != TCL_OK || !list) {
        return {};
    }

    Tcl_Size size = 0;
    Tcl_Obj** values = nullptr;
    if (Tcl_ListObjGetElements(nullptr, list, &size, &values) != TCL_OK || size < 4 ||
        object_text(values[0]) != "VINC") {
        return {};
    }

    constexpr Tcl_Size maximum_records = config::error_chain_max_depth;
    constexpr Tcl_Size record_fields = 5;
    Tcl_WideInt version = 0;
    Tcl_WideInt count = 0;
    Tcl_WideInt truncated = 0;
    if (Tcl_GetWideIntFromObj(nullptr, values[1], &version) != TCL_OK || version != 1 ||
        Tcl_GetWideIntFromObj(nullptr, values[2], &count) != TCL_OK || count < 1 || count > maximum_records ||
        Tcl_GetWideIntFromObj(nullptr, values[3], &truncated) != TCL_OK ||
        (truncated != 0 && truncated != 1) || (truncated && count < 2) || size != 4 + count * record_fields) {
        return {};
    }

    std::array<ErrorRecord, maximum_records> records{};
    for (Tcl_Size index = 0; index < count; ++index) {
        std::array<Tcl_WideInt, record_fields> fields{};
        for (Tcl_Size field = 0; field < record_fields; ++field) {
            if (Tcl_GetWideIntFromObj(nullptr, values[4 + index * record_fields + field], &fields[field]) !=
                TCL_OK) {
                return {};
            }
        }

        if (fields[0] < 0 || fields[0] > UINT32_MAX || fields[3] < 0 || fields[3] > UINT32_MAX ||
            fields[1] < INT32_MIN || fields[1] > INT32_MAX || fields[2] < INT32_MIN ||
            fields[2] > INT32_MAX) {
            return {};
        }

        records[index] = {static_cast<ErrorDomain>(fields[0]),
                          static_cast<std::int32_t>(fields[1]),
                          static_cast<std::int32_t>(fields[2]),
                          {static_cast<NativeErrorSource>(fields[3]), fields[4]}};
    }

    const auto& current = records[0];
    return Error::from_raw(current.domain, current.code, current.operation, current.native,
                           std::span(records).subspan(1, static_cast<std::size_t>(count - 1)),
                           truncated != 0);
}

int call_original_command(Tcl_Interp* interpreter_handle, const char* name, Tcl_Size count,
                          Tcl_Obj* const values[]) {
    std::vector<Tcl_Obj*> arguments(values, values + count);
    arguments[0] = Tcl_NewStringObj(name, -1);
    // Borrow the incoming arguments; own only the replacement command name
    // until the synchronous Tcl evaluation has finished.
    Tcl_IncrRefCount(arguments[0]);
    const int result = Tcl_EvalObjv(interpreter_handle, count, arguments.data(), TCL_EVAL_DIRECT);
    Tcl_DecrRefCount(arguments[0]);
    return result;
}
}

Result<std::filesystem::path> executable_directory() {
    std::lock_guard lock(tcl_initialization_mutex);
    if (library_path.empty()) {
        return std::unexpected(Error{ErrorCode::TclResourcesMissing, ErrorOperation::InitializeTcl});
    }
    return library_path.parent_path().parent_path();
}

Result<void> initialize_tcl(const char* executable) {
    if (!executable || !*executable) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::InitializeTcl});
    }

    std::lock_guard lock(tcl_initialization_mutex);
    if (!library_path.empty()) {
        return {};
    }

    Tcl_FindExecutable(executable);
    const auto* name = Tcl_GetNameOfExecutable();
    if (!name || !*name) {
        return std::unexpected(Error{ErrorCode::TclResourcesMissing, ErrorOperation::InitializeTcl});
    }
    auto executable_path = osal::path_from_utf8(name);
    if (!executable_path) {
        return std::unexpected(wrap_error(ErrorCode::TclInitializationFailed, ErrorOperation::InitializeTcl,
                                          executable_path.error()));
    }

    const auto candidate = executable_path->parent_path() / "lib" / "tcl9.1";
    std::error_code filesystem_error;
    const bool exists = std::filesystem::is_regular_file(candidate / "init.tcl", filesystem_error);
    if (!exists) {
        return std::unexpected(Error{ErrorCode::TclResourcesMissing, ErrorOperation::InitializeTcl,
                                     osal::native_error(filesystem_error)});
    }
    // Commit readiness only after validation. A failed attempt remains retryable.
    library_path = candidate;
    return {};
}

Result<std::vector<std::string>> tcl_reserved_commands() {
    ScriptDiagnostic diagnostic;
    auto created = create_interpreter(diagnostic);
    if (!created) {
        return std::unexpected(created.error());
    }

    auto interpreter_handle = std::move(*created);
    if (Tcl_Eval(interpreter_handle.get(), "concat [info commands] [array names auto_index]") != TCL_OK) {
        return std::unexpected(Error{ErrorCode::TclInitializationFailed, ErrorOperation::ReserveCommands});
    }

    Tcl_Size count = 0;
    Tcl_Obj** values = nullptr;
    if (Tcl_ListObjGetElements(interpreter_handle.get(), Tcl_GetObjResult(interpreter_handle.get()), &count,
                               &values) != TCL_OK) {
        return std::unexpected(Error{ErrorCode::TclInitializationFailed, ErrorOperation::ReserveCommands});
    }

    std::vector<std::string> result{"srun", "scancel"};
    for (Tcl_Size i = 0; i < count; ++i) {
        result.push_back(object_text(values[i]));
    }

    return result;
}

std::string script_text(const ScriptReport& report) {
    if (report.outcome) {
        return report.outcome->shutdown ? "Script requested framework shutdown"
                                        : "Script completed: " + report.diagnostic.file;
    }

    if (report.outcome.error().is(ErrorCode::Cancelled)) {
        return "Script canceled: " + report.diagnostic.file;
    }

    return bounded_diagnostic(
        "Script error in " + report.diagnostic.file + " (line " + std::to_string(report.diagnostic.line) +
        "):\n" + vinc::error_to_string(report.outcome.error()) + "\n" + report.diagnostic.details);
}

struct TclRuntime::Impl {
    std::future<ScriptReport> job;
    std::mutex mutex;
    std::condition_variable wake;
    // Tcl evaluation stays on the worker. This pointer is only published so another
    // thread can request cancellation while the same mutex prevents interpreter deletion.
    Tcl_Interp* live_interpreter = nullptr; // guarded against deletion while cancel is called
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
        constexpr auto limit = config::shell_script_output_bytes;
        auto byte_count = (std::min)(text.size(), limit - output.size());
        while (byte_count && byte_count < text.size() &&
               (static_cast<unsigned char>(text[byte_count]) & 0xc0) == 0x80) {
            --byte_count;
        }

        output.append(text.substr(0, byte_count));
        truncated |= byte_count != text.size();
    }

    bool cancel() {
        std::lock_guard lock(mutex);
        canceled.store(true);
        wake.notify_all();
        stop.request_stop();
        if (live_interpreter) {
            Tcl_CancelEval(live_interpreter, nullptr, nullptr, TCL_CANCEL_UNWIND);
        }

        return true;
    }

    int check(Tcl_Interp* interpreter_handle) {
        if (!canceled.load()) {
            return TCL_OK;
        }

        Tcl_CancelEval(interpreter_handle, nullptr, nullptr, TCL_CANCEL_UNWIND);
        return error(interpreter_handle, "Script canceled");
    }

    // Replace only the blocking sleep form with a cancellable wait. Delegate timer/event
    // forms to Tcl so its normal event-loop semantics remain available.
    int handle_after(Tcl_Interp* interpreter_handle, Tcl_Size count, Tcl_Obj* const values[]) {
        Tcl_WideInt remaining_ms;
        if (count == 2 && Tcl_GetWideIntFromObj(nullptr, values[1], &remaining_ms) == TCL_OK) {
            // Short waits avoid overflowing chrono for arbitrarily large Tcl values.
            std::unique_lock lock(mutex);
            while (remaining_ms > 0 && !canceled.load()) {
                const auto slice =
                    (std::min)(remaining_ms, Tcl_WideInt{config::shell_script_wait_slice.count()});
                wake.wait_for(lock, std::chrono::milliseconds(slice), [&] {
                    return canceled.load();
                });
                remaining_ms -= slice;
            }

            lock.unlock();
            return check(interpreter_handle);
        }

        return call_original_command(interpreter_handle, "::vinc::native_after", count, values);
    }

    int handle_puts(Tcl_Interp* interpreter_handle, Tcl_Size count, Tcl_Obj* const values[]) {
        Tcl_Size index = 1;
        bool newline = true;
        if (index < count && object_text(values[index]) == "-nonewline") {
            newline = false;
            ++index;
        }

        std::string channel = "stdout";
        if (count - index == 2) {
            channel = object_text(values[index++]);
        }

        if (count - index != 1 || (channel != "stdout" && channel != "stderr")) {
            return call_original_command(interpreter_handle, "::vinc::native_puts", count, values);
        }

        write(object_text(values[index]));
        if (newline) {
            write("\n");
        }

        return TCL_OK;
    }

    int dispatch_command(Tcl_Interp* interpreter_handle, const std::string& name, Tcl_Size count,
                         Tcl_Obj* const values[]) {
        // Tcl reaches module names through unknown. Keep Tcl's original unknown as a
        // fallback for autoloaded commands that the framework does not recognize.
        const Tcl_Size first = name == "unknown" ? 1 : 0;
        std::vector<std::string> arguments;
        for (auto i = first; i < count; ++i) {
            arguments.push_back(object_text(values[i]));
        }

        if (!arguments.empty()) {
            while (arguments.front().starts_with("::")) {
                arguments.front().erase(0, 2);
            }
        }

        if (name != "unknown" && !arguments.empty()) {
            arguments.front() = name;
        }
        auto result = dispatch(arguments, stop.get_token());
        if (check(interpreter_handle) != TCL_OK) {
            return TCL_ERROR;
        }

        if (!result) {
            if (name == "unknown" && result.error().is(ErrorCode::UnknownCommand)) {
                return call_original_command(interpreter_handle, "::vinc::native_unknown", count, values);
            }

            return framework_error(interpreter_handle, result.error());
        }

        if (result->code != 0) {
            // A nonzero application result remains a value in the C++ API. Tcl's
            // existing command policy exposes it as an ordinary script error.
            error(interpreter_handle, result->message);
            Tcl_SetErrorCode(interpreter_handle, "VINC_APPLICATION", std::to_string(result->code).c_str(),
                             nullptr);
            return TCL_ERROR;
        }

        if (result->shutdown) {
            shutdown.store(true);
            cancel();
            return check(interpreter_handle);
        }

        if (!result->message.empty()) {
            write(result->message);
            write("\n");
        }

        Tcl_SetObjResult(interpreter_handle, Tcl_NewIntObj(0));
        return TCL_OK;
    }

    // Bridge already-parsed Tcl objects directly to argument vectors. No command-line
    // reparse occurs, so spaces, empty arguments and literal metacharacters survive.
    enum class CommandKind { Unknown, Puts, After, Exit, Help, Run, Cancel };

    template<CommandKind kind>
    static int command(void* data, Tcl_Interp* interpreter_handle, Tcl_Size count,
                       Tcl_Obj* const values[]) noexcept {
        auto& self = *static_cast<Impl*>(data);
        try {
            if (self.check(interpreter_handle) != TCL_OK) {
                return TCL_ERROR;
            }

            if (kind == CommandKind::Exit) {
                if (count != 1) {
                    return framework_error(interpreter_handle,
                                           Error{ErrorCode::InvalidArgument, ErrorOperation::Execute});
                }

                self.shutdown.store(true);
                self.cancel();
                return self.check(interpreter_handle);
            }

            if (kind == CommandKind::Cancel) {
                if (count != 1) {
                    return framework_error(interpreter_handle,
                                           Error{ErrorCode::InvalidArgument, ErrorOperation::Execute});
                }

                self.cancel();
                return self.check(interpreter_handle);
            }

            if (kind == CommandKind::Run) {
                return framework_error(interpreter_handle,
                                       Error{ErrorCode::Busy, ErrorOperation::StartScript});
            }

            if (kind == CommandKind::After) {
                return self.handle_after(interpreter_handle, count, values);
            }

            if (kind == CommandKind::Puts) {
                return self.handle_puts(interpreter_handle, count, values);
            }

            // Callback identity survives Tcl rename; argv[0] is only the spelling used by the caller.
            return self.dispatch_command(interpreter_handle,
                                         kind == CommandKind::Unknown ? "unknown" : "help", count, values);
        } catch (const std::exception& failure) {
            return framework_error(interpreter_handle,
                                   Error{ErrorCode::UnexpectedException, ErrorOperation::Execute},
                                   failure.what());
        } catch (...) {
            return framework_error(interpreter_handle,
                                   Error{ErrorCode::UnexpectedException, ErrorOperation::Execute});
        }
    }

    Result<void> install_app_directory(Tcl_Interp* interpreter_handle) {
        auto directory = executable_directory();
        if (!directory) {
            return std::unexpected(wrap_error(ErrorCode::TclInitializationFailed,
                                              ErrorOperation::InstallCommands, directory.error()));
        }

        auto encoded_directory = tcl_path(*directory);
        if (!encoded_directory) {
            return std::unexpected(wrap_error(ErrorCode::TclInitializationFailed,
                                              ErrorOperation::InstallCommands, encoded_directory.error()));
        }

        ObjectOwner app_directory(Tcl_NewStringObj(encoded_directory->data(),
                                                   static_cast<Tcl_Size>(encoded_directory->size())));
        if (!Tcl_SetVar2Ex(interpreter_handle, "::vinc::app_dir", nullptr, app_directory.value, TCL_GLOBAL_ONLY)) {
            return std::unexpected(
                Error{ErrorCode::TclInitializationFailed, ErrorOperation::InstallCommands});
        }

        return {};
    }

    // Commands and argument objects are interpreter-owned. The caller retains
    // the interpreter until all setup, evaluation and cancellation detachment finish.
    Result<void> install_commands(Tcl_Interp* interpreter_handle) {
        if (Tcl_Eval(interpreter_handle,
                     "namespace eval ::vinc {}; rename unknown ::vinc::native_unknown; rename puts "
                     "::vinc::native_puts; rename after ::vinc::native_after") != TCL_OK) {
            return std::unexpected(
                Error{ErrorCode::TclInitializationFailed, ErrorOperation::InstallCommands});
        }

        Tcl_CreateObjCommand2(interpreter_handle, "unknown", command<CommandKind::Unknown>, this, nullptr);
        Tcl_CreateObjCommand2(interpreter_handle, "puts", command<CommandKind::Puts>, this, nullptr);
        Tcl_CreateObjCommand2(interpreter_handle, "after", command<CommandKind::After>, this, nullptr);
        Tcl_CreateObjCommand2(interpreter_handle, "exit", command<CommandKind::Exit>, this, nullptr);
        Tcl_CreateObjCommand2(interpreter_handle, "help", command<CommandKind::Help>, this, nullptr);
        Tcl_CreateObjCommand2(interpreter_handle, "srun", command<CommandKind::Run>, this, nullptr);
        Tcl_CreateObjCommand2(interpreter_handle, "scancel", command<CommandKind::Cancel>, this, nullptr);

        return install_app_directory(interpreter_handle);
    }

    Result<void> install_arguments(Tcl_Interp* interpreter_handle, const std::string& file,
                                   const std::vector<std::string>& arguments) {
        ObjectOwner file_object(Tcl_NewStringObj(file.data(), static_cast<Tcl_Size>(file.size())));
        if (!Tcl_SetVar2Ex(interpreter_handle, "argv0", nullptr, file_object.value, TCL_GLOBAL_ONLY)) {
            return std::unexpected(
                Error{ErrorCode::TclInitializationFailed, ErrorOperation::InstallArguments});
        }

        ObjectOwner list(Tcl_NewListObj(0, nullptr));
        for (const auto& arg : arguments) {
            ObjectOwner value(Tcl_NewStringObj(arg.data(), static_cast<Tcl_Size>(arg.size())));
            if (Tcl_ListObjAppendElement(interpreter_handle, list.value, value.value) != TCL_OK) {
                return std::unexpected(
                    Error{ErrorCode::TclInitializationFailed, ErrorOperation::InstallArguments});
            }
        }

        if (!Tcl_SetVar2Ex(interpreter_handle, "argv", nullptr, list.value, TCL_GLOBAL_ONLY)) {
            return std::unexpected(
                Error{ErrorCode::TclInitializationFailed, ErrorOperation::InstallArguments});
        }

        ObjectOwner count(Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(arguments.size())));
        if (!Tcl_SetVar2Ex(interpreter_handle, "argc", nullptr, count.value, TCL_GLOBAL_ONLY)) {
            return std::unexpected(
                Error{ErrorCode::TclInitializationFailed, ErrorOperation::InstallArguments});
        }

        return {};
    }

    int evaluate_source(Tcl_Interp* interpreter_handle, const ScriptSource& source, const std::string& file) {
        if (!source.text) {
            return Tcl_EvalFile(interpreter_handle, file.c_str());
        }

        // Public Tcl command establishes a diagnostic identity, not a real file.
        // Nested source restores it. These objects are retained until synchronous
        // evaluation returns; the interpreter retains its own script-name reference.
        ObjectOwner info(Tcl_NewStringObj("info", -1));
        ObjectOwner script(Tcl_NewStringObj("script", -1));
        ObjectOwner name(Tcl_NewStringObj(file.data(), static_cast<Tcl_Size>(file.size())));
        Tcl_Obj* command[] = {info.value, script.value, name.value};
        const int installed = Tcl_EvalObjv(interpreter_handle, 3, command, TCL_EVAL_DIRECT);
        if (installed != TCL_OK) {
            return installed;
        }

        return Tcl_EvalEx(interpreter_handle, source.text->data(),
                          static_cast<Tcl_Size>(source.text->size()), 0);
    }

    // Create, evaluate and destroy this interpreter on the same worker thread.
    // No variables, procedures or timers are carried into the next script run.
    ScriptReport run(const ScriptSource& source, const std::vector<std::string>& arguments) {
        // Finalize only this worker's Tcl state, after interpreter destruction.
        struct TclThreadGuard {
            ~TclThreadGuard() {
                Tcl_FinalizeThread();
            }
        } thread_guard;

        ScriptDiagnostic diagnostic;
        try {
            const auto encoded = tcl_path(source.path);
            if (!encoded) {
                return {std::unexpected(
                            wrap_error(ErrorCode::ScriptFailed, ErrorOperation::RunScript, encoded.error())),
                        std::move(diagnostic)};
            }

            const auto& file = *encoded;
            diagnostic.file = bounded_diagnostic(file);
            auto created = create_interpreter(diagnostic);
            if (!created) {
                return {std::unexpected(created.error()), std::move(diagnostic)};
            }

            auto interpreter_handle = std::move(*created);
            auto installed = install_commands(interpreter_handle.get());
            if (!installed) {
                capture_diagnostic(interpreter_handle.get(), diagnostic);
                return {std::unexpected(installed.error()), std::move(diagnostic)};
            }

            auto arguments_installed = install_arguments(interpreter_handle.get(), file, arguments);
            if (!arguments_installed) {
                capture_diagnostic(interpreter_handle.get(), diagnostic);
                return {std::unexpected(arguments_installed.error()), std::move(diagnostic)};
            }
            {
                std::lock_guard lock(mutex);
                live_interpreter = interpreter_handle.get();
            }

            // Detach before deleting the interpreter, including exception paths.
            struct InterpreterPublicationGuard {
                Impl& self;

                ~InterpreterPublicationGuard() {
                    std::lock_guard lock(self.mutex);
                    self.live_interpreter = nullptr;
                }
            } publication_guard{*this};

            int code = TCL_ERROR;
            if (!canceled.load()) {
                code = evaluate_source(interpreter_handle.get(), source, file);
            }

            if (shutdown.load()) {
                return {ScriptCompletion{true}, std::move(diagnostic)};
            }

            if (canceled.load()) {
                return {std::unexpected(Error{ErrorCode::Cancelled, ErrorOperation::RunScript}),
                        std::move(diagnostic)};
            }

            if (code != TCL_OK) {
                capture_diagnostic(interpreter_handle.get(), diagnostic);
                const auto cause = framework_cause(interpreter_handle.get(), code);
                const auto failure =
                    cause ? *cause : Error{ErrorCode::ScriptFailed, ErrorOperation::RunScript};
                return {std::unexpected(failure), std::move(diagnostic)};
            }

            return {ScriptCompletion{}, std::move(diagnostic)};
        } catch (const std::exception& failure) {
            diagnostic.details = bounded_diagnostic(failure.what());
            return {std::unexpected(Error{ErrorCode::UnexpectedException, ErrorOperation::RunScript}),
                    std::move(diagnostic)};
        }
    }
};

TclRuntime::TclRuntime() : impl_(std::make_unique<Impl>()) {
}

TclRuntime::~TclRuntime() {
    if (active()) {
        cancel();
        impl_->job.wait();
    }
}

Result<void> TclRuntime::start(std::filesystem::path path, std::vector<std::string> arguments,
                               Dispatch dispatch) {
    return start_source({std::move(path), std::nullopt}, std::move(arguments), std::move(dispatch));
}

Result<void> TclRuntime::start_source(ScriptSource source, std::vector<std::string> arguments,
                                    Dispatch dispatch) {
    if (active()) {
        return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::StartScript});
    }

    impl_->canceled.store(false);
    impl_->shutdown.store(false);
    impl_->stop = {};
    impl_->dispatch = std::move(dispatch);
    try {
        impl_->job =
            std::async(std::launch::async, [this, source = std::move(source), arguments = std::move(arguments)] {
                return impl_->run(source, arguments);
            });
    } catch (const std::system_error& failure) {
        return std::unexpected(
            Error{ErrorCode::WorkerFailed, ErrorOperation::StartWorker, osal::native_error(failure.code())});
    }

    return {};
}

bool TclRuntime::active() const {
    return impl_->job.valid();
}

bool TclRuntime::ready() const {
    return active() && impl_->job.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

bool TclRuntime::cancel() {
    return active() && impl_->cancel();
}

ScriptReport TclRuntime::finish() {
    if (!active()) {
        vinc::contract_violation("TclRuntime::finish requires an owned job");
    }

    return impl_->job.get();
}

std::string TclRuntime::take_output() {
    std::lock_guard lock(impl_->mutex);
    auto result = std::move(impl_->output);
    impl_->output.clear();
    if (impl_->truncated) {
        result += "\n[Script output truncated]\n";
        impl_->truncated = false;
    }

    return result;
}
}

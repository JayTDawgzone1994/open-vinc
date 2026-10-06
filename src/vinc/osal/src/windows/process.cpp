#include <vinc/osal/detail/native_error.hpp>
// Windows process backend: UTF-16 launch data, explicit handle inheritance and exit observation.
// Prepare ownership before CreateProcess so a successfully created child is never lost.

#include <vinc/osal/detail/native.hpp>
#include <vinc/osal/detail/process_pipe_native.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <limits>
#include <map>
#include <utility>

namespace vinc::osal::detail {
namespace {
// Poll children whose native thread-pool wait could not be registered.
constexpr DWORD fallback_poll_interval_ms = 50;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {
    }

    ~Handle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {
    }

    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            Handle old(std::exchange(value_, std::exchange(other.value_, nullptr)));
        }

        return *this;
    }

    HANDLE get() const noexcept {
        return value_;
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

private:
    HANDLE value_;
};

vinc::Result<std::wstring> widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring{};
    }

    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::Spawn});
    }

    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (!length) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
    }

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             result.data(), length)) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
    }

    return result;
}

// Microsoft CRT/wmain quoting, including empty arguments and trailing slashes.
std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result.push_back(ch);
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

struct EnvironmentLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                    static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};

vinc::Result<std::vector<wchar_t>> environment_block(const SpawnOptions& options,
                                                     const ChildPipe* child_pipe) {
    std::map<std::wstring, std::wstring, EnvironmentLess> entries;
    if (options.environment_mode == EnvironmentMode::Inherit) {
        struct EnvironmentStrings {
            LPWCH value = GetEnvironmentStringsW();

            ~EnvironmentStrings() {
                if (value) {
                    FreeEnvironmentStringsW(value);
                }
            }
        } inherited;

        if (!inherited.value) {
            return std::unexpected(
                native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
        }

        for (const wchar_t* cursor = inherited.value; *cursor;) {
            std::wstring entry(cursor);
            cursor += entry.size() + 1;
            // Preserve Windows' special '=C:' working-directory entries.
            const auto split = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
            if (split != std::wstring::npos) {
                entries[entry.substr(0, split)] = entry.substr(split + 1);
            }
        }
    }

    for (const auto& entry : options.environment) {
        auto name = widen(entry.name);
        if (!name) {
            return std::unexpected(name.error());
        }

        if (!entry.value) {
            entries.erase(*name);
        } else {
            auto value = widen(*entry.value);
            if (!value) {
                return std::unexpected(value.error());
            }
            entries[*name] = std::move(*value);
        }
    }
    entries.erase(L"VINC_OSAL_PIPE_BOOTSTRAP");
    if (child_pipe && !child_pipe->inheritance().empty()) {
        auto bootstrap = widen(child_pipe->bootstrap());
        if (!bootstrap) {
            return std::unexpected(bootstrap.error());
        }
        entries[L"VINC_OSAL_PIPE_BOOTSTRAP"] = std::move(*bootstrap);
    }

    std::vector<wchar_t> block;
    for (const auto& [name, value] : entries) {
        block.insert(block.end(), name.begin(), name.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }

    if (block.empty()) {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

vinc::Result<Handle> standard_handle(StdioMode mode, DWORD standard, bool input,
                                     std::intptr_t captured = -1) {
    if (mode == StdioMode::Capture) {
        HANDLE duplicate = nullptr;
        // captured is a HANDLE encoded by the Pipe backend in intptr_t.
        // Restore its pointer-sized token for Win32; do not dereference it.
        if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(captured), GetCurrentProcess(),
                             &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            return std::unexpected(
                native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
        }

        return Handle(duplicate);
    }

    if (mode == StdioMode::Inherit) {
        const HANDLE source = GetStdHandle(standard);
        if (source && source != INVALID_HANDLE_VALUE) {
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, TRUE,
                                 DUPLICATE_SAME_ACCESS)) {
                return std::unexpected(
                    native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
            }

            return Handle(duplicate);
        }
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE null_file =
        CreateFileW(L"NUL", input ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_file == INVALID_HANDLE_VALUE) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
    }

    return Handle(null_file);
}
} // namespace

char pipe_platform_tag() noexcept {
    return 'w';
}

void close_pipe_transfer(std::intptr_t handle) noexcept {
    // This pointer-sized token came from the Windows Pipe backend.
    CloseHandle(reinterpret_cast<HANDLE>(handle));
}

vinc::Result<std::intptr_t> duplicate_pipe_transfer(std::intptr_t source) {
    HANDLE duplicate = nullptr;
    // source stores an opaque HANDLE in intptr_t; only Win32 interprets it.
    if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(source), GetCurrentProcess(),
                         &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
    }

    // Preserve the complete HANDLE token for the matching native close/adopt path.
    return reinterpret_cast<std::intptr_t>(duplicate);
}

vinc::Result<std::string> read_pipe_bootstrap() {
    const DWORD length = GetEnvironmentVariableW(L"VINC_OSAL_PIPE_BOOTSTRAP", nullptr, 0);
    if (!length) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::NotFound, vinc::osal::ErrorOperation::AdoptPipe});
    }

    if (length > max_pipe_bootstrap_bytes + 1) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::AdoptPipe});
    }

    std::wstring text(length, L'\0');
    const DWORD count = GetEnvironmentVariableW(L"VINC_OSAL_PIPE_BOOTSTRAP", text.data(), length);
    if (!count || count >= length) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::AdoptPipe});
    }

    std::string result;
    result.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        if (text[i] > 127) {
            return std::unexpected(
                vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::AdoptPipe});
        }
        result.push_back(static_cast<char>(text[i]));
    }

    return result;
}

vinc::Error native_error(vinc::osal::ErrorOperation operation, int value) noexcept {
    vinc::osal::ErrorCode code = vinc::osal::ErrorCode::SystemError;
    switch (static_cast<DWORD>(value)) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DIRECTORY:
        code = vinc::osal::ErrorCode::NotFound;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        code = vinc::osal::ErrorCode::PermissionDenied;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_TOO_MANY_OPEN_FILES:
        code = vinc::osal::ErrorCode::ResourceExhausted;
        break;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        code = vinc::osal::ErrorCode::Unsupported;
        break;
    case ERROR_INVALID_PARAMETER:
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_NO_UNICODE_TRANSLATION:
        code = vinc::osal::ErrorCode::InvalidArgument;
        break;
    default:
        break;
    }

    return {code, operation, {vinc::NativeErrorSource::Win32, static_cast<std::uint32_t>(value)}};
}

struct MonitorImpl {
    Handle wake_event;
};

NativeMonitor::NativeMonitor(std::unique_ptr<MonitorImpl> impl) : impl_(std::move(impl)) {
}

NativeMonitor::~NativeMonitor() = default;

vinc::Result<std::unique_ptr<NativeMonitor>> NativeMonitor::create() {
    auto impl = std::make_unique<MonitorImpl>();
    impl->wake_event = Handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!impl->wake_event.get()) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::InitializeService, static_cast<int>(GetLastError())));
    }

    return std::unique_ptr<NativeMonitor>(new NativeMonitor(std::move(impl)));
}

void NativeMonitor::wake() noexcept {
    SetEvent(impl_->wake_event.get());
}

vinc::Result<void> NativeMonitor::wait(bool polling) noexcept {
    const auto result =
        WaitForSingleObject(impl_->wake_event.get(), polling ? fallback_poll_interval_ms : INFINITE);
    if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT) {
        return {};
    }

    return std::unexpected(
        native_error(vinc::osal::ErrorOperation::Observe, static_cast<int>(GetLastError())));
}

struct ProcessImpl {
    Handle process;
    DWORD pid = 0;
    HANDLE wake_event = nullptr; // monitor outlives every registered process
    PTP_WAIT wait = nullptr;

    ~ProcessImpl() {
        if (wait) {
            SetThreadpoolWait(wait, nullptr, nullptr);
            WaitForThreadpoolWaitCallbacks(wait, TRUE);
            CloseThreadpoolWait(wait);
        }
    }

    static void CALLBACK on_exit(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) noexcept {
        SetEvent(static_cast<ProcessImpl*>(context)->wake_event);
    }
};

NativeProcess::NativeProcess(std::unique_ptr<ProcessImpl> impl) : impl_(std::move(impl)) {
}

NativeProcess::~NativeProcess() = default;

std::uint64_t NativeProcess::pid() const noexcept {
    return impl_->pid;
}

bool NativeProcess::event_driven() const noexcept {
    return true;
}

namespace {
// CreateProcessW limits the command line to 32,767 UTF-16 characters, including NUL.
constexpr std::size_t maximum_command_characters = 32767;

vinc::Result<std::wstring> build_command_line(const SpawnOptions& options) {
    std::wstring command = quote(options.executable.native());
    for (const auto& argument : options.arguments) {
        auto wide = widen(argument);
        if (!wide) {
            return std::unexpected(wide.error());
        }

        command += L' ';
        command += quote(*wide);
    }

    if (command.size() >= maximum_command_characters) {
        return std::unexpected(
            vinc::Error{vinc::osal::ErrorCode::InvalidArgument, vinc::osal::ErrorOperation::Spawn});
    }

    return command;
}

// The caller owns the backing storage and destroys this guard before that storage.
struct AttributeList {
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;

    ~AttributeList() {
        if (value) {
            DeleteProcThreadAttributeList(value);
        }
    }
};

// All buffers remain in spawn's scope through CreateProcessW. On failure, only
// an initialized list is deleted; inherited handles remain owned by their callers.
vinc::Result<void> prepare_inheritance(std::vector<std::byte>& storage, AttributeList& attributes,
                                       std::vector<HANDLE>& handles, const Handle& input,
                                       const Handle& output, const Handle& error,
                                       const ChildPipe* child_pipe) {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    storage.resize(bytes);

    // Win32 supplies the opaque layout and required byte count. The default
    // allocator provides native heap alignment; only Win32 accesses the list.
    // storage is never resized after initialization and outlives AttributeList.
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
    }

    attributes.value = list;
    handles = {input.get(), output.get(), error.get()};
    if (child_pipe) {
        for (auto handle : child_pipe->inheritance()) {
            // ChildPipe stores native HANDLE values in pointer-sized integers;
            // restore the opaque Win32 token without dereferencing it.
            handles.push_back(reinterpret_cast<HANDLE>(handle));
        }
    }

    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                   handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
    }

    return {};
}

// ProcessImpl owns the registration, including rollback before any child exists.
vinc::Result<void> register_exit_wait(ProcessImpl& process, HANDLE wake_event) {
    process.wake_event = wake_event;
    process.wait = CreateThreadpoolWait(ProcessImpl::on_exit, &process, nullptr);
    if (!process.wait) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
    }

    return {};
}

vinc::Result<void> launch_process(ProcessImpl& owner, const SpawnOptions& options, std::wstring& command,
                                  std::vector<wchar_t>& environment, STARTUPINFOEXW& startup) {
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(options.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                            (options.new_process_group ? CREATE_NEW_PROCESS_GROUP : 0),
                        environment.data(),
                        options.working_directory ? options.working_directory->c_str() : nullptr,
                        &startup.StartupInfo, &process)) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::Spawn, static_cast<int>(GetLastError())));
    }

    owner.process = Handle(process.hProcess);
    owner.pid = process.dwProcessId;
    CloseHandle(process.hThread);
    SetThreadpoolWait(owner.wait, process.hProcess, nullptr);

    return {};
}
} // namespace

vinc::Result<std::unique_ptr<NativeProcess>>
NativeProcess::spawn(const SpawnOptions& options, NativeMonitor& monitor, const ChildPipe* child_pipe) {
    auto command = build_command_line(options);
    if (!command) {
        return std::unexpected(command.error());
    }

    auto environment = environment_block(options, child_pipe);
    if (!environment) {
        return std::unexpected(environment.error());
    }

    auto input = standard_handle(options.stdin_mode, STD_INPUT_HANDLE, true);
    if (!input) {
        return std::unexpected(input.error());
    }

    auto output = standard_handle(options.stdout_mode, STD_OUTPUT_HANDLE, false,
                                  child_pipe ? child_pipe->output(0) : -1);
    if (!output) {
        return std::unexpected(output.error());
    }

    auto error = standard_handle(options.stderr_mode, STD_ERROR_HANDLE, false,
                                 child_pipe ? child_pipe->output(1) : -1);
    if (!error) {
        return std::unexpected(error.error());
    }

    // Declaration order preserves cleanup: handle list, native attributes,
    // backing bytes, then the three duplicated standard handles.
    std::vector<std::byte> attribute_storage;
    AttributeList attributes;
    std::vector<HANDLE> handles;
    auto inheritance =
        prepare_inheritance(attribute_storage, attributes, handles, *input, *output, *error, child_pipe);
    if (!inheritance) {
        return std::unexpected(inheritance.error());
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input->get();
    startup.StartupInfo.hStdOutput = output->get();
    startup.StartupInfo.hStdError = error->get();
    startup.lpAttributeList = attributes.value;

    // Allocate every owning object and callback registration before creating a
    // child. No fallible allocation is permitted after CreateProcess succeeds.
    auto native = std::unique_ptr<NativeProcess>(new NativeProcess(std::make_unique<ProcessImpl>()));
    auto registered = register_exit_wait(*native->impl_, monitor.implementation().wake_event.get());
    if (!registered) {
        return std::unexpected(registered.error());
    }

    auto launched = launch_process(*native->impl_, options, *command, *environment, startup);
    if (!launched) {
        return std::unexpected(launched.error());
    }

    return native;
}

vinc::Result<std::optional<ExitStatus>> NativeProcess::poll() noexcept {
    const auto wait = WaitForSingleObject(impl_->process.get(), 0);
    if (wait == WAIT_TIMEOUT) {
        return std::optional<ExitStatus>{};
    }

    if (wait != WAIT_OBJECT_0) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::Observe, static_cast<int>(GetLastError())));
    }

    DWORD code;
    if (!GetExitCodeProcess(impl_->process.get(), &code)) {
        return std::unexpected(
            native_error(vinc::osal::ErrorOperation::Observe, static_cast<int>(GetLastError())));
    }

    return std::optional(ExitStatus{ExitKind::Code, code});
}

vinc::Result<TerminateResult> NativeProcess::terminate() noexcept {
    // Conventional Windows STATUS_CONTROL_C_EXIT value; not a crash classifier.
    if (TerminateProcess(impl_->process.get(), 0xc000013aUL)) {
        return TerminateResult::Requested;
    }

    const auto error = static_cast<int>(GetLastError());
    if (WaitForSingleObject(impl_->process.get(), 0) == WAIT_OBJECT_0) {
        return TerminateResult::AlreadyExited;
    }

    return std::unexpected(native_error(vinc::osal::ErrorOperation::Terminate, error));
}

} // namespace vinc::osal::detail

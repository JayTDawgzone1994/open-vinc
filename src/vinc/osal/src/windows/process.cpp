// Windows process backend: UTF-16 launch data, explicit handle inheritance and exit observation.
// Prepare ownership before CreateProcess so a successfully created child is never lost.

#include "../native.hpp"
#include "../process_pipe_native.hpp"

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

Result<std::wstring> widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring{};
    }

    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Spawn});
    }

    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (!length) {
        return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
    }

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             result.data(), length)) {
        return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
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

Result<std::vector<wchar_t>> environment_block(const SpawnOptions& options, const ChildPipe* child_pipe) {
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
            return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
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

Result<Handle> standard_handle(StdioMode mode, DWORD standard, bool input, std::intptr_t captured = -1) {
    if (mode == StdioMode::Capture) {
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(captured), GetCurrentProcess(),
                             &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
        }

        return Handle(duplicate);
    }

    if (mode == StdioMode::Inherit) {
        const HANDLE source = GetStdHandle(standard);
        if (source && source != INVALID_HANDLE_VALUE) {
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, TRUE,
                                 DUPLICATE_SAME_ACCESS)) {
                return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
            }

            return Handle(duplicate);
        }
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE null_file =
        CreateFileW(L"NUL", input ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_file == INVALID_HANDLE_VALUE) {
        return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
    }

    return Handle(null_file);
}
} // namespace

char pipe_platform_tag() noexcept {
    return 'w';
}

void close_pipe_transfer(std::intptr_t handle) noexcept {
    CloseHandle(reinterpret_cast<HANDLE>(handle));
}

Result<std::intptr_t> duplicate_pipe_transfer(std::intptr_t source) {
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(source), GetCurrentProcess(),
                         &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
    }

    return reinterpret_cast<std::intptr_t>(duplicate);
}

Result<std::string> read_pipe_bootstrap() {
    const DWORD length = GetEnvironmentVariableW(L"VINC_OSAL_PIPE_BOOTSTRAP", nullptr, 0);
    if (!length) {
        return std::unexpected(OsError{ErrorCode::NotFound, Operation::AdoptPipe});
    }

    if (length > max_pipe_bootstrap_bytes + 1) {
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::AdoptPipe});
    }

    std::wstring text(length, L'\0');
    const DWORD count = GetEnvironmentVariableW(L"VINC_OSAL_PIPE_BOOTSTRAP", text.data(), length);
    if (!count || count >= length) {
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::AdoptPipe});
    }

    std::string result;
    result.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        if (text[i] > 127) {
            return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::AdoptPipe});
        }
        result.push_back(static_cast<char>(text[i]));
    }

    return result;
}

OsError native_error(Operation operation, int value) noexcept {
    ErrorCode code = ErrorCode::SystemError;
    switch (static_cast<DWORD>(value)) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DIRECTORY:
        code = ErrorCode::NotFound;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        code = ErrorCode::PermissionDenied;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_TOO_MANY_OPEN_FILES:
        code = ErrorCode::ResourceExhausted;
        break;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        code = ErrorCode::Unsupported;
        break;
    case ERROR_INVALID_PARAMETER:
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_NO_UNICODE_TRANSLATION:
        code = ErrorCode::InvalidArgument;
        break;
    default:
        break;
    }

    return {code, operation, {value, std::system_category()}};
}

struct MonitorImpl {
    Handle wake_event;
};

NativeMonitor::NativeMonitor(std::unique_ptr<MonitorImpl> impl) : impl_(std::move(impl)) {
}

NativeMonitor::~NativeMonitor() = default;

Result<std::unique_ptr<NativeMonitor>> NativeMonitor::create() {
    auto impl = std::make_unique<MonitorImpl>();
    impl->wake_event = Handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!impl->wake_event.get()) {
        return std::unexpected(native_error(Operation::InitializeService, static_cast<int>(GetLastError())));
    }

    return std::unique_ptr<NativeMonitor>(new NativeMonitor(std::move(impl)));
}

void NativeMonitor::wake() noexcept {
    SetEvent(impl_->wake_event.get());
}

Result<void> NativeMonitor::wait(bool polling) noexcept {
    const auto result =
        WaitForSingleObject(impl_->wake_event.get(), polling ? fallback_poll_interval_ms : INFINITE);
    if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT) {
        return {};
    }

    return std::unexpected(native_error(Operation::Observe, static_cast<int>(GetLastError())));
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

Result<std::unique_ptr<NativeProcess>>
NativeProcess::spawn(const SpawnOptions& options, NativeMonitor& monitor, const ChildPipe* child_pipe) {
    std::wstring command = quote(options.executable.native());
    for (const auto& argument : options.arguments) {
        auto wide = widen(argument);
        if (!wide) {
            return std::unexpected(wide.error());
        }
        command += L' ';
        command += quote(*wide);
    }

    if (command.size() >= 32767) {
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Spawn});
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

    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> attribute_storage(bytes);

    struct AttributeList {
        LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;

        ~AttributeList() {
            if (value) {
                DeleteProcThreadAttributeList(value);
            }
        }
    } attributes;

    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) {
        return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
    }
    attributes.value = list;
    std::vector<HANDLE> handles{input->get(), output->get(), error->get()};
    if (child_pipe) {
        for (auto handle : child_pipe->inheritance()) {
            handles.push_back(reinterpret_cast<HANDLE>(handle));
        }
    }

    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                   handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
        return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input->get();
    startup.StartupInfo.hStdOutput = output->get();
    startup.StartupInfo.hStdError = error->get();
    startup.lpAttributeList = list;

    // Allocate every owning object and callback registration before creating a
    // child. No fallible allocation is permitted after CreateProcess succeeds.
    auto native = std::unique_ptr<NativeProcess>(new NativeProcess(std::make_unique<ProcessImpl>()));
    native->impl_->wake_event = monitor.implementation().wake_event.get();
    native->impl_->wait = CreateThreadpoolWait(ProcessImpl::on_exit, native->impl_.get(), nullptr);
    if (!native->impl_->wait) {
        return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(options.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                            (options.new_process_group ? CREATE_NEW_PROCESS_GROUP : 0),
                        environment->data(),
                        options.working_directory ? options.working_directory->c_str() : nullptr,
                        &startup.StartupInfo, &process)) {
        return std::unexpected(native_error(Operation::Spawn, static_cast<int>(GetLastError())));
    }
    native->impl_->process = Handle(process.hProcess);
    native->impl_->pid = process.dwProcessId;
    CloseHandle(process.hThread);
    SetThreadpoolWait(native->impl_->wait, process.hProcess, nullptr);
    return native;
}

Result<std::optional<ExitStatus>> NativeProcess::poll() noexcept {
    const auto wait = WaitForSingleObject(impl_->process.get(), 0);
    if (wait == WAIT_TIMEOUT) {
        return std::optional<ExitStatus>{};
    }

    if (wait != WAIT_OBJECT_0) {
        return std::unexpected(native_error(Operation::Observe, static_cast<int>(GetLastError())));
    }
    DWORD code;
    if (!GetExitCodeProcess(impl_->process.get(), &code)) {
        return std::unexpected(native_error(Operation::Observe, static_cast<int>(GetLastError())));
    }

    return std::optional(ExitStatus{ExitKind::Code, code});
}

Result<TerminateResult> NativeProcess::terminate() noexcept {
    // Conventional Windows STATUS_CONTROL_C_EXIT value; not a crash classifier.
    if (TerminateProcess(impl_->process.get(), 0xc000013aUL)) {
        return TerminateResult::Requested;
    }

    const auto error = static_cast<int>(GetLastError());
    if (WaitForSingleObject(impl_->process.get(), 0) == WAIT_OBJECT_0) {
        return TerminateResult::AlreadyExited;
    }

    return std::unexpected(native_error(Operation::Terminate, error));
}

} // namespace vinc::osal::detail

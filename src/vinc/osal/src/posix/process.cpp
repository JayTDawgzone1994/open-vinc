// POSIX process backend using posix_spawn and waitpid for OSAL-owned children.
// Native readiness wakes the observer; waitpid remains the authority for the final exit result.

#include "platform.hpp"
#include "../process_pipe_native.hpp"

#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <fcntl.h>
#include <map>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(VINC_OSAL_MACOS)
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace vinc::osal::detail {

char pipe_platform_tag() noexcept { return 'p'; }
void close_pipe_transfer(std::intptr_t handle) noexcept { ::close(static_cast<int>(handle)); }
Result<std::intptr_t> duplicate_pipe_transfer(std::intptr_t source) {
    const int duplicate = fcntl(static_cast<int>(source), F_DUPFD_CLOEXEC, first_pipe_source_fd);
    if (duplicate < 0) return std::unexpected(native_error(Operation::Spawn, errno));
    return static_cast<std::intptr_t>(duplicate);
}
Result<std::string> read_pipe_bootstrap() {
    const char* value = std::getenv(pipe_bootstrap_name);
    if (!value) return std::unexpected(OsError{ErrorCode::NotFound, Operation::AdoptPipe});
    return std::string(value);
}

OsError native_error(Operation operation, int value) noexcept {
    ErrorCode code = ErrorCode::SystemError;
    switch (value) {
    case ENOENT: case ENOTDIR: code = ErrorCode::NotFound; break;
    case EACCES: case EPERM: code = ErrorCode::PermissionDenied; break;
    case ENOMEM: case EMFILE: case ENFILE: case EAGAIN: code = ErrorCode::ResourceExhausted; break;
    case ENOSYS: case ENOTSUP: code = ErrorCode::Unsupported; break;
    case EINVAL: case E2BIG: case ENOEXEC: code = ErrorCode::InvalidArgument; break;
    case ECHILD: code = ErrorCode::ObservationLost; break;
    default: break;
    }
    return {code, operation, {value, std::generic_category()}};
}

struct ProcessImpl {
    pid_t pid = -1;
    int watch = -1;
    bool monitored = false;
    ~ProcessImpl() { if (watch >= 0) release_watch(watch); }
};
NativeProcess::NativeProcess(std::unique_ptr<ProcessImpl> impl) : impl_(std::move(impl)) {}
NativeProcess::~NativeProcess() = default;
std::uint64_t NativeProcess::pid() const noexcept { return static_cast<std::uint64_t>(impl_->pid); }
bool NativeProcess::event_driven() const noexcept { return impl_->monitored; }

Result<std::unique_ptr<NativeProcess>> NativeProcess::spawn(const SpawnOptions& options, NativeMonitor& monitor, const ChildPipe* child_pipe) {
    // The embedding application must not auto-reap or independently wait for
    // OSAL-owned children. Do not replace a host's global SIGCHLD disposition.
    struct sigaction disposition{};
    if (sigaction(SIGCHLD, nullptr, &disposition) != 0)
        return std::unexpected(native_error(Operation::Spawn, errno));
    if (disposition.sa_handler == SIG_IGN || (disposition.sa_flags & SA_NOCLDWAIT))
        return std::unexpected(OsError{ErrorCode::InvalidArgument, Operation::Spawn});

    std::map<std::string, std::string> environment;
    if (options.environment_mode == EnvironmentMode::Inherit) {
#if defined(VINC_OSAL_MACOS)
        char** inherited = *_NSGetEnviron();
#else
        char** inherited = environ;
#endif
        for (char** cursor = inherited; cursor && *cursor; ++cursor) {
            std::string entry(*cursor);
            const auto split = entry.find('=');
            if (split != std::string::npos) environment[entry.substr(0, split)] = entry.substr(split + 1);
        }
    }
    for (const auto& entry : options.environment) {
        if (entry.value) environment[entry.name] = *entry.value;
        else environment.erase(entry.name);
    }
    environment.erase(pipe_bootstrap_name);
    if (child_pipe && !child_pipe->inheritance().empty()) environment[pipe_bootstrap_name] = child_pipe->bootstrap();
    std::vector<std::string> environment_strings;
    environment_strings.reserve(environment.size());
    for (const auto& [name, value] : environment) environment_strings.push_back(name + "=" + value);
    std::vector<char*> envp;
    envp.reserve(environment_strings.size() + 1);
    for (auto& entry : environment_strings) envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::vector<std::string> arguments;
    arguments.reserve(options.arguments.size() + 1);
    arguments.push_back(options.executable.native());
    arguments.insert(arguments.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);

    struct Actions {
        posix_spawn_file_actions_t value{};
        bool initialized = false;
        ~Actions() { if (initialized) posix_spawn_file_actions_destroy(&value); }
    } actions;
    struct Attributes {
        posix_spawnattr_t value{};
        bool initialized = false;
        ~Attributes() { if (initialized) posix_spawnattr_destroy(&value); }
    } attributes;
    int error = posix_spawn_file_actions_init(&actions.value);
    if (error) return std::unexpected(native_error(Operation::Spawn, error));
    actions.initialized = true;
    error = posix_spawnattr_init(&attributes.value);
    if (error) return std::unexpected(native_error(Operation::Spawn, error));
    attributes.initialized = true;

    const StdioMode modes[]{options.stdin_mode, options.stdout_mode, options.stderr_mode};
    for (int fd = 0; fd < 3; ++fd) {
        if (modes[fd] == StdioMode::Capture) {
            // Sources are duplicated above all bootstrap targets. Apply stdio
            // redirection before closefrom and before handing off generic pipes.
            error = posix_spawn_file_actions_adddup2(&actions.value,
                static_cast<int>(child_pipe->output(static_cast<std::size_t>(fd - 1))), fd);
            if (error) return std::unexpected(native_error(Operation::Spawn, error));
            continue;
        }
        bool inherit = modes[fd] == StdioMode::Inherit;
        if (inherit && fcntl(fd, F_GETFD) == -1) {
            if (errno != EBADF) return std::unexpected(native_error(Operation::Spawn, errno));
            inherit = false; // missing standard streams explicitly become /dev/null
        }
        error = inherit ? posix_spawn_file_actions_adddup2(&actions.value, fd, fd)
            : posix_spawn_file_actions_addopen(&actions.value, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0);
        if (error) return std::unexpected(native_error(Operation::Spawn, error));
    }
    if (options.working_directory) {
#if defined(VINC_OSAL_HAS_SPAWN_CHDIR)
        error = posix_spawn_file_actions_addchdir_np(&actions.value, options.working_directory->c_str());
        if (error) return std::unexpected(native_error(Operation::Spawn, error));
#else
        return std::unexpected(OsError{ErrorCode::Unsupported, Operation::Spawn});
#endif
    }

    if (child_pipe) {
        const auto inherited = child_pipe->inheritance();
        for (std::size_t i = 0; i < inherited.size(); ++i) {
            error = posix_spawn_file_actions_adddup2(&actions.value, static_cast<int>(inherited[i]), 3 + static_cast<int>(i));
            if (error) return std::unexpected(native_error(Operation::Spawn, error));
        }
    }
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (options.new_process_group) {
        flags |= POSIX_SPAWN_SETPGROUP;
        error = posix_spawnattr_setpgroup(&attributes.value, 0);
        if (error) return std::unexpected(native_error(Operation::Spawn, error));
    }
#if defined(VINC_OSAL_MACOS)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
    // glibc >= 2.34, including Ubuntu 22.04. Close even descriptors opened by
    // other threads, instead of enumerating the parent's descriptor table.
    error = posix_spawn_file_actions_addclosefrom_np(&actions.value, child_pipe ? 3 + static_cast<int>(child_pipe->inheritance().size()) : 3);
    if (error) return std::unexpected(native_error(Operation::Spawn, error));
#endif
    sigset_t mask;
    sigemptyset(&mask);
    error = posix_spawnattr_setsigmask(&attributes.value, &mask);
    if (error) return std::unexpected(native_error(Operation::Spawn, error));
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    error = posix_spawnattr_setsigdefault(&attributes.value, &defaults);
    if (error) return std::unexpected(native_error(Operation::Spawn, error));
    error = posix_spawnattr_setflags(&attributes.value, flags);
    if (error) return std::unexpected(native_error(Operation::Spawn, error));

    auto native = std::unique_ptr<NativeProcess>(new NativeProcess(std::make_unique<ProcessImpl>()));
    error = posix_spawn(&native->impl_->pid, options.executable.c_str(), &actions.value, &attributes.value, argv.data(), envp.data());
    if (error) return std::unexpected(native_error(Operation::Spawn, error));
    // No allocations after spawning. If event registration fails (including a
    // very fast exit on macOS), retain ownership and use waitpid/WNOHANG polling.
    // Only OSAL may reap these PIDs, so a live or unreaped child cannot be reused.
    auto watch = watch_process(monitor.implementation(), native->impl_->pid);
    if (watch) { native->impl_->watch = *watch; native->impl_->monitored = true; }
    return native;
}

Result<std::optional<ExitStatus>> NativeProcess::poll() noexcept {
    int status = 0;
    pid_t result;
    do { result = waitpid(impl_->pid, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == 0) return std::optional<ExitStatus>{};
    if (result < 0) return std::unexpected(native_error(Operation::Observe, errno));
    if (WIFEXITED(status)) return std::optional(ExitStatus{ExitKind::Code, static_cast<std::uint32_t>(WEXITSTATUS(status))});
    if (WIFSIGNALED(status)) {
        bool dumped = false;
#ifdef WCOREDUMP
        dumped = WCOREDUMP(status);
#endif
        return std::optional(ExitStatus{ExitKind::Signal, static_cast<std::uint32_t>(WTERMSIG(status)), dumped});
    }
    return std::optional<ExitStatus>{};
}
Result<TerminateResult> NativeProcess::terminate() noexcept { return kill_process(impl_->pid, impl_->watch); }

} // namespace vinc::osal::detail

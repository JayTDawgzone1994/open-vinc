// Linux fault coverage. Limits and unlink interception affect this process only.
#include <vinc/osal/detail/shared_memory_create.hpp>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

namespace {
using namespace vinc::osal;

enum class UnlinkMode { Native, Denied, AlreadyRemoved };
UnlinkMode unlink_mode = UnlinkMode::Native;
unsigned unlink_calls = 0;
constexpr std::uint64_t memory_bytes = 4096;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ZeroSoftLimit {
public:
    explicit ZeroSoftLimit(int resource) : resource_(resource) {
        require(::getrlimit(resource_, &original_) == 0, "getrlimit");
        const rlimit limit{0, original_.rlim_max};
        require(::setrlimit(resource_, &limit) == 0, "setrlimit");
    }

    ~ZeroSoftLimit() {
        (void)::setrlimit(resource_, &original_);
    }

    ZeroSoftLimit(const ZeroSoftLimit&) = delete;
    ZeroSoftLimit& operator=(const ZeroSoftLimit&) = delete;
private:
    int resource_;
    rlimit original_{};
};

void check_create_failure(const vinc::Result<SharedMemory>& result, ErrorCode code, int native) {
    require(!result && result.error().is(code), "wrong primary error");
    require(result.error().is_operation(ErrorOperation::CreateSharedMemory), "wrong primary operation");
    require(result.error().native.source == vinc::NativeErrorSource::LinuxErrno &&
                result.error().native.value() == native, "wrong primary errno");
}

void require_absent(const std::string& name) {
    auto opened = SharedMemory::open(name, SharedMemoryAccess::ReadOnly);
    require(!opened && opened.error().is(ErrorCode::NotFound), "expected name to be absent");
}

void exercise(const std::string& name) {
    detail::SharedMemoryCreateAttempt attempt;
    {
        ZeroSoftLimit limit(RLIMIT_NOFILE);
        auto failed = attempt.create(name, memory_bytes);
        check_create_failure(failed, ErrorCode::ResourceExhausted, EMFILE);
        require(!attempt.name_cleanup_error && unlink_calls == 0, "unowned name scheduled for removal");
    }
    require_absent(name);

    // A zero file-size limit makes the kernel reject ftruncate after shm_open
    // succeeded. Ignore the signal so the errno is observable by OSAL.
    require(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR, "ignore SIGXFSZ");
    for (auto mode : {UnlinkMode::Native, UnlinkMode::Denied, UnlinkMode::AlreadyRemoved}) {
        unlink_mode = mode;
        unlink_calls = 0;
        {
            ZeroSoftLimit limit(RLIMIT_FSIZE);
            auto failed = attempt.create(name, memory_bytes);
            check_create_failure(failed, ErrorCode::InvalidArgument, EFBIG);
        }
        unlink_mode = UnlinkMode::Native;
        require(unlink_calls == 1, "rollback must attempt one unlink");
        if (mode == UnlinkMode::Denied) {
            const auto& cleanup = attempt.name_cleanup_error;
            require(cleanup && cleanup->is(ErrorCode::PermissionDenied), "missing cleanup failure");
            require(cleanup->is_operation(ErrorOperation::UnlinkSharedMemory), "wrong cleanup operation");
            require(cleanup->native.source == vinc::NativeErrorSource::LinuxErrno &&
                        cleanup->native.value() == EACCES, "wrong cleanup errno");
            auto remaining = SharedMemory::open(name, SharedMemoryAccess::ReadOnly);
            require(remaining.has_value(), "reported leftover was not discoverable");
            require(SharedMemory::unlink_name(name).has_value(), "recover owned name");
            auto invalid = attempt.create(name, 0);
            require(!invalid && invalid.error().is(ErrorCode::InvalidArgument), "validation failed");
        }

        require(!attempt.name_cleanup_error, "stale cleanup failure");
        require_absent(name);
    }
    auto retry = attempt.create(name, memory_bytes);
    require(retry.has_value() && !attempt.name_cleanup_error, "retry did not recover");
    require(SharedMemory::unlink_name(name).has_value(), "remove successful retry");
}
} // namespace

extern "C" int __real_shm_unlink(const char* name);
extern "C" int __wrap_shm_unlink(const char* name) {
    ++unlink_calls;
    if (unlink_mode == UnlinkMode::Denied) {
        errno = EACCES;
        return -1;
    }

    if (unlink_mode == UnlinkMode::AlreadyRemoved) {
        // Remove the real name first so the following native unlink sees ENOENT.
        if (__real_shm_unlink(name) != 0) {
            return -1;
        }
    }
    return __real_shm_unlink(name);
}

int main() {
    const std::string name = "rollback_" + std::to_string(::getpid());
    try {
        require_absent(name);
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }

    try {
        exercise(name);
        return 0;
    } catch (const std::exception& failure) {
        unlink_mode = UnlinkMode::Native;
        (void)SharedMemory::unlink_name(name);
        std::cerr << failure.what() << '\n';
        return 1;
    }
}

#pragma once

#include <vinc/detail/resource_names.hpp>
#include <vinc/osal/file_lock.hpp>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

namespace vinc::detail {
// Central-owned instance admission and process-crash journal. No Module maps or
// owns this object. The instance lock outlives every resource-producing service.
class Instance final : public ResourceNames {
public:
    static Result<std::shared_ptr<Instance>> create(std::string name);
    ~Instance();
    Result<std::string> reserve() override;
    Result<void> retire(std::string_view name) override;
    const std::string& name() const noexcept;
    const std::filesystem::path& journal() const noexcept;

private:
    Instance() = default;
    Result<void> prepare_directory();
    Result<void> acquire_instance();
    Result<void> clean_previous();
    Result<void> create_epoch();
    std::string name_;
    std::string epoch_;
    std::filesystem::path root_;
    std::filesystem::path epoch_path_;
    osal::FileLock file_;
    std::optional<osal::FileLockGuard> guard_;
    std::mutex mutex_;
    std::uint64_t next_ = 1;
    std::size_t recorded_ = 0;
};
}

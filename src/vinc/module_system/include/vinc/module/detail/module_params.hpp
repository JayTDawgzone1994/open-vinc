#pragma once
#include <vinc/param/detail/access.hpp>
#include <vinc/param/detail/storage.hpp>
#include <vinc/module/detail/module_locks.hpp>
#include <condition_variable>
#include <optional>

namespace vinc::detail {
// One Host-owned client, fed by the existing watchdog. It owns no I/O thread.
class ModuleParams final : public param::detail::Access {
public:
    ModuleParams(std::shared_ptr<control::Session> session, ModuleLocks& locks);
    void identify(std::string name);
    void stop() noexcept;
    Result<void> receive(control::Frame frame);

    std::string_view module_name() const noexcept override {
        return name_;
    }

    Result<void> register_value(std::string name, param::Value defaults,
                                param::Permission permission) override;
    Result<void> set_value(std::string name, param::Value value) override;
    Result<void> watch_value(std::string name, param::Type type) override;
    Result<std::optional<param::Snapshot>> snapshot(std::uint64_t known_revision) override;

private:
    struct Pending {
        std::uint16_t operation;
        std::optional<control::Bytes> reply;
        std::condition_variable_any changed;
    };

    std::shared_ptr<control::Session> session_;
    ModuleLocks& locks_;
    std::string name_;
    std::stop_source stop_;
    std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<Pending>> pending_;
    std::uint64_t next_request_ = 1;
    std::mutex read_mutex_;
    std::uint64_t lock_id_ = 0;
    osal::SharedMemoryMapping mapping_;
    control::Bytes buffer_;
    Result<control::Bytes> request(std::uint16_t operation, control::Bytes payload);
    Result<void> attach();
    Result<void> map_image(std::string_view name);
};
}

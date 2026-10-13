#include <vinc/module/base_module.hpp>
#include <vinc/module/detail/context_lock.hpp>
#include <cstdlib>

// Test-only framework service seam. The public Context deliberately has no lock API.
class LockFixture final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
    std::unique_ptr<vinc::detail::ContextLock> held_;
public:
    LockFixture() : BaseModule("lock") {
    }
    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (args.empty()) {
            return 0;
        }
        if (args[0] == "crash") {
            std::_Exit(91);
        }
        if (args[0] == "publish" || args[0] == "release") {
            if (!held_) {
                return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                                  vinc::module_system::ErrorOperation::AccessLock});
            }
            auto result = args[0] == "publish" ? held_->publish_and_unlock() : held_->unlock();
            if (!result) {
                return std::unexpected(result.error());
            }
            held_.reset();
            return 0;
        }
        const auto id = std::stoull(args.at(1));
        auto acquired = vinc::detail::ContextLockAccess::lock(context, id, (vinc::Deadline::max)());
        if (!acquired) {
            return std::unexpected(acquired.error());
        }
        const auto publication = (*acquired)->publication();
        if (args[0] == "hold") {
            held_ = std::move(*acquired);
        }
        return publication ? 1 : 0;
    }
    vinc::Result<void> on_stop() override {
        held_.reset();
        return {};
    }
};
REGISTER_MODULE(LockFixture, "LockFixture")

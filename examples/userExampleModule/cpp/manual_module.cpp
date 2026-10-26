#include <vinc/vinc.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

class UserManualModule final : public vinc::BaseModule<vinc::RunMode::Manual> {
public:
    UserManualModule() : BaseModule("manualCpp") {
    }

    vinc::Result<void> on_start(const std::vector<std::string>& args) override {
        // Use "loop" for the framework's cancellable idle wait, otherwise pump one batch at a time.
        use_loop_ = std::find(args.begin(), args.end(), "loop") != args.end();
        return {};
    }

    vinc::Result<void> main() override {
        if (use_loop_) {
            return context.pumpLoop();
        }

        constexpr auto application_idle_interval = std::chrono::milliseconds(10);
        while (context.living() && !finished_) {
            auto batch = context.pumpOnce();
            if (!batch) {
                return std::unexpected(batch.error());
            }

            if (batch->stopped) {
                break;
            }

            if (batch->callbacks != 0 || batch->commands != 0 || batch->lost_messages != 0) {
                std::cout << "PUMP callbacks=" << batch->callbacks << " commands=" << batch->commands
                          << " lost=" << batch->lost_messages << '\n';
            }

            // Put bounded application work here. Do not spin while there is no work.
            std::this_thread::sleep_for(application_idle_interval);
        }

        // Successful return is normal completion, not a request to restart this Module.
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (std::find(args.begin(), args.end(), "finish") != args.end() && !use_loop_) {
            finished_ = true;
            return 0;
        }

        std::cout << "MANUAL " << get_module_name() << " living=" << context.living() << '\n';
        return 0;
    }

private:
    bool use_loop_ = false;
    bool finished_ = false;
};

REGISTER_MODULE(UserManualModule, "userManualModule")

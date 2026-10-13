#include "support.hpp"
#include <vinc/module/base_module.hpp>
#include <thread>

namespace {
using namespace vinc;
using namespace param_test;

class ParamModule final : public BaseModule<RunMode::FrameworkLoop> {
    std::thread::id pump_thread_;
    std::int32_t callbacks_ = 0;
    bool throw_callback_ = false;
    bool watcher_ = false;

public:
    ParamModule() : BaseModule("camera") {
    }

    Result<std::string> on_name(std::vector<std::string>& args) override {
        check(!args.empty(), "name argument required");
        auto name = args.front();
        args.erase(args.begin());
        return name;
    }

    Result<void> on_start(const std::vector<std::string>& args) override {
        pump_thread_ = std::this_thread::get_id();
        watcher_ = get_module_name() == "watcher";
        if (watcher_) {
            take(context.param.register_param<std::int32_t>("camera.open"));
            take(context.param.register_param<std::int32_t>("camera.fixed"));
            take(context.param.register_param<std::int32_t>("future.value"));
            std::int32_t unchanged = 123;
            auto absent = context.param.update("future.value", unchanged);
            check(!absent && absent.error().is(param::ErrorCode::NotFound) && unchanged == 123,
                  "missing cache never touches destination");
        } else if (get_module_name() == "future") {
            take(context.param.register_param<std::int32_t>("value", 42));
        } else {
            if (!args.empty() && args[0] == "double") {
                take(context.param.register_param<double>("open", 2.5, param::Permission::Open));
            } else {
                const auto defaults = args.empty() ? 1 : std::stoi(args[0]);
                take(context.param.register_param<std::int32_t>("open", defaults, param::Permission::Open));
            }
            take(context.param.register_param<std::int32_t>("fixed", 5));
            auto duplicate = context.param.register_param<std::int32_t>("fixed", 9);
            check(!duplicate && duplicate.error().is(param::ErrorCode::AlreadyRegistered),
                  "duplicate registration");
        }
        return context.param.on_change([this](const param::ParamMap& values) {
            check(std::this_thread::get_id() == pump_thread_, "Param callback must use pump thread");
            ++callbacks_;
            if (throw_callback_) {
                throw std::runtime_error("intentional user callback failure");
            }
            if (watcher_) {
                const auto absent = values.find("future.value");
                if (absent == values.end()) {
                    std::int32_t ignored = 0;
                    check(!context.param.update("future.value", ignored),
                          "missing entry absent from callback map");
                }
            }
            // Reentrant cache reads do not lock the callback or fetch remotely.
            for (const auto& [name, entry] : values) {
                if (const auto* value = std::get_if<std::int32_t>(&entry.value)) {
                    std::int32_t local = 0;
                    take(context.param.update(name, local));
                    check(local == *value, "callback map matches local cache");
                }
            }
        });
    }

    Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        check(!args.empty(), "command required");
        if (args[0] == "get") {
            std::int32_t value = 0;
            take(context.param.update(args.at(1), value));
            return value;
        }
        if (args[0] == "missing") {
            for (const auto& watch : context.param.watches()) {
                if (watch.name == args.at(1)) {
                    return watch.present ? 0 : 1;
                }
            }
            return -1;
        }
        if (args[0] == "denied") {
            auto changed = context.param.set<std::int32_t>(args.at(1), 99);
            return !changed && changed.error().is(param::ErrorCode::PermissionDenied) ? 1 : 0;
        }
        if (args[0] == "write") {
            take(context.param.set<std::int32_t>(args.at(1), std::stoi(args.at(2))));
            return 0;
        }
        if (args[0] == "coalesce") {
            std::int32_t previous = 0;
            take(context.param.update("open", previous));
            take(context.param.set<std::int32_t>("open", 10));
            take(context.param.set<std::int32_t>("open", 20));
            take(context.param.set<std::int32_t>("open", 30));
            std::int32_t cached = 0;
            take(context.param.update("open", cached));
            check(cached == previous, "set acknowledgement must not mutate pump cache");
            return callbacks_;
        }
        if (args[0] == "callbacks") {
            return callbacks_;
        }
        if (args[0] == "arm") {
            throw_callback_ = true;
            return 0;
        }
        if (args[0] == "add") {
            take(context.param.register_param<std::int32_t>("extra", 8));
            return 0;
        }

        return -1;
    }
};

static_assert(!std::is_default_constructible_v<param::Params>);
}
REGISTER_MODULE(ParamModule, "paramTest")

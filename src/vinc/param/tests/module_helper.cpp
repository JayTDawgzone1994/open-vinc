#include "support.hpp"
#include <vinc/module/base_module.hpp>
#include <thread>
#include <future>

namespace {
using namespace vinc;
using namespace param_test;

class ParamModule final : public BaseModule<RunMode::FrameworkLoop> {
    std::thread::id pump_thread_;
    std::int32_t callbacks_ = 0;
    bool throw_callback_ = false;
    bool watcher_ = false;
    std::int32_t effective_gain_ = 5;

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
        } else if (get_module_name() == "startup") {
            take(context.param.register_param<std::int32_t>("gain", 5));
            auto value = take(take(context.param.read_current("gain")).get<std::int32_t>());
            if (!args.empty()) {
                check(args.size() == 2 && args[0] == "--gain", "startup args owned by Module");
                value = std::stoi(args[1]);
                check(value >= 0, "startup gain must be nonnegative");
                take(context.param.set<std::int32_t>("gain", value));
            }
            if (value < 0) {
                // A typed but unusable stored value is corrected, not file corruption.
                value = 5;
                take(context.param.set<std::int32_t>("gain", value));
            }
            effective_gain_ = value;
        } else if (get_module_name() == "future") {
            take(context.param.register_param<std::int32_t>("value", 42));
        } else {
            if (!args.empty() && args[0] == "double") {
                take(context.param.register_param<double>("open", 2.5, {.permission = param::Permission::Public}));
            } else {
                const auto defaults = args.empty() ? 1 : std::stoi(args[0]);
                take(context.param.register_param<std::int32_t>("open", defaults, {.permission = param::Permission::Public}));
            }
            const auto hint = !args.empty() && args[0] == "double" ? param::RestartHint::App : param::RestartHint::Module;
            take(context.param.register_param<std::int32_t>("fixed", 5, {.restart_hint = hint}));
            auto current = take(context.param.read_current("fixed"));
            check(current.restart_hint == hint && current.permission == param::Permission::Limited,
                  "restart-only options preserve Limited across Host/Plugin");
            check(std::get<std::int32_t>(current.default_value) == 5,
                  "startup read preserves registered default");
            const auto committed = take(current.get<std::int32_t>());
            current.value = std::int32_t{999};
            check(take(take(context.param.read_current("fixed")).get<std::int32_t>()) == committed,
                  "current read returns an independent Entry");
            std::int32_t untouched = 123;
            const auto uncached = context.param.update("fixed", untouched);
            check(!uncached && uncached.error().is(param::ErrorCode::NotFound) && untouched == 123,
                  "startup read must not populate pump cache");
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
                const auto current = take(context.param.read_current(name));
                check(current.version >= entry.version, "callback may read current state without deadlock");
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
        if (args[0] == "effective") {
            return effective_gain_;
        }
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
            const auto callbacks_before = callbacks_;
            check(take(take(context.param.read_current("open")).get<std::int32_t>()) == 30,
                  "current read sees committed change before next pump");
            check(callbacks_ == callbacks_before, "current read never dispatches callbacks");
            std::int32_t cached = 0;
            take(context.param.update("open", cached));
            check(cached == previous, "set acknowledgement must not mutate pump cache");
            return callbacks_;
        }
        if (args[0] == "current") {
            const auto watches_before = context.param.watches().size();
            const auto current = take(context.param.read_current(args.at(1)));
            check(context.param.watches().size() == watches_before, "current read does not add watches");
            return current.get<std::int32_t>();
        }
        if (args[0] == "parallel_reads") {
            constexpr std::size_t reader_count = 4;
            constexpr std::size_t reads_per_thread = 16;
            std::vector<std::future<Result<param::Entry>>> readers;
            for (std::size_t reader = 0; reader < reader_count; ++reader) {
                readers.push_back(std::async(std::launch::async, [this] -> Result<param::Entry> {
                    Result<param::Entry> current;
                    for (std::size_t read = 0; read < reads_per_thread; ++read) {
                        current = context.param.read_current("open");
                        if (!current) {
                            return current;
                        }
                    }
                    return current;
                }));
            }
            for (auto& reader : readers) {
                check(take(take(reader.get()).get<std::int32_t>()) == 30, "concurrent read is complete");
            }
            return 0;
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

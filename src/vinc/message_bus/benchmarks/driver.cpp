#include <benchmark.hpp>
#include <vinc/module/host/module_manager.hpp>
#include <vinc/osal/path.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace vinc;
using namespace benchmark;

template <class T>
bool good(const Result<T>& result) {
    if (!result) {
        std::cerr << error_to_string(result.error()) << '\n';
        return false;
    }

    return true;
}

bool healthy(ModuleManager& manager) {
    for (const auto& module : manager.snapshots()) {
        if (module.state == ModuleState::Failed || module.state == ModuleState::Stopped) {
            std::cerr << module.name << " exited before measurement finished\n";
            if (module.last_failure) {
                std::cerr << error_to_string(module.last_failure->error) << '\n';
            }

            return false;
        }
    }

    return true;
}

bool run(ModuleManager& manager, const std::filesystem::path& plugin,
         std::vector<std::string> args, const Options& options, std::uint64_t receivers) {
    ModuleType type{"busBenchmark", plugin, {}};
    if (!good(manager.register_module_type(type))) {
        return false;
    }

    std::vector<MID> subscribers;
    for (std::uint64_t index = 0; index < receivers; ++index) {
        auto subscriber_args = args;
        subscriber_args[0] = "subscriber";
        if (receivers > 1) {
            const auto directory = options.directory / ("subscriber-" + std::to_string(index));
            if (!std::filesystem::create_directory(directory)) {
                std::cerr << "refusing an existing subscriber output directory\n";
                return false;
            }
            auto path = osal::path_to_utf8(directory);
            if (!good(path)) {
                return false;
            }
            subscriber_args[7] = *path;
        }
        subscriber_args.push_back("--name");
        subscriber_args.push_back("benchmark_subscriber_" + std::to_string(index));
        auto subscriber = manager.start_module(type.name, subscriber_args);
        if (!good(subscriber) || !good(manager.wait_started(*subscriber))) {
            return false;
        }
        subscribers.push_back(*subscriber);
    }

    args[0] = "publisher";
    auto publisher = manager.start_module(type.name, args);
    if (!good(publisher) || !good(manager.wait_started(*publisher))) {
        return false;
    }

    // Allow initial topology processing before starting warmup. Initial misses
    // still invalidate the pilot; this delay is not a delivery guarantee.
    std::this_thread::sleep_for(warmup_drain);
    auto go = manager.command(*publisher, {"go"}, control::Clock::now() + command_timeout);
    if (!good(go) || *go != 0) {
        return false;
    }

    const auto expected = std::chrono::nanoseconds(options.period_ns * (options.samples + options.warmup));
    const auto deadline = control::Clock::now() + expected * trial_deadline_multiplier + trial_deadline_margin;
    while (!std::filesystem::exists(options.directory / "publisher_resources.csv")) {
        if (control::Clock::now() >= deadline || !healthy(manager) ||
            std::filesystem::exists(options.directory / "cancel")) {
            std::cerr << "publisher failed or exceeded the bounded trial deadline\n";
            return false;
        }

        std::this_thread::sleep_for(control_poll);
    }

    // Confirm files are closed and main has returned to pumping before reading.
    auto done = manager.command(*publisher, {"done"}, control::Clock::now() + command_timeout);
    if (!good(done) || *done != 1) {
        return false;
    }

    const auto drain_end = control::Clock::now() + drain_timeout;
    for (const auto subscriber : subscribers) {
        while (control::Clock::now() < drain_end) {
            auto count = manager.command(subscriber, {"count"}, control::Clock::now() + command_timeout);
            if (!good(count)) {
                return false;
            }
            if (static_cast<std::uint64_t>(*count) == options.samples + options.warmup) {
                break;
            }
            std::this_thread::sleep_for(control_poll);
        }
        auto saved = manager.command(subscriber, {"save"}, control::Clock::now() + command_timeout);
        if (!good(saved) || *saved != 0) {
            return false;
        }
    }
    return healthy(manager);
}

int entry(const std::vector<std::string>& argv) {
    if (argv.size() != 9 && argv.size() != 10) {
        std::cerr << "usage: bus_benchmark plugin push|loan bytes slots period_ns samples warmup output_directory [subscribers]\n";
        return 2;
    }

    std::uint64_t receivers = 1;
    if (argv.size() == 10 && (!number(argv[9], receivers) || receivers > config::message_bus_default_subscribers_per_topic)) {
        std::cerr << "invalid subscriber count\n";
        return 2;
    }
    std::vector<std::string> args(argv.begin() + 1, argv.begin() + 9);
    args[0] = "publisher";
    Options options;
    if (!parse(args, options) || !std::filesystem::is_directory(options.directory)) {
        std::cerr << "invalid arguments or missing output directory\n";
        return 2;
    }

    for (const auto* name : {"sent.csv", "received.csv", "publisher_resources.csv", "cancel"}) {
        if (std::filesystem::exists(options.directory / name)) {
            std::cerr << "refusing an output directory containing earlier trial data\n";
            return 2;
        }
    }

    auto plugin = osal::path_from_utf8(argv[1]);
    if (!plugin) {
        std::cerr << error_to_string(plugin.error()) << '\n';
        return 2;
    }

    const auto start = now_ns();
    const auto before = resources();
    message_bus::Limits limits;
    limits.max_shared_bytes = shared_budget;
    auto created = ModuleManager::create(limits);
    if (!created) {
        std::cerr << error_to_string(created.error()) << '\n';
        return 1;
    }

    auto manager = std::move(*created);
    const bool success = run(*manager, std::filesystem::absolute(*plugin), args, options, receivers);
    manager->begin_shutdown();
    const bool stopped = good(manager->wait_shutdown(control::Clock::now() + shutdown_timeout));
    const bool recorded = save_resources(options.directory / "central_resources.csv", before, resources(), start, now_ns());
    return success && stopped && recorded ? 0 : 1;
}
}

int guarded_entry(const std::vector<std::string>& args) {
    try {
        return entry(args);
    } catch (const std::exception& error) {
        // Last boundary for allocator/filesystem failures in the benchmark tool.
        // Manager unwinding still supervises and reaps its own child processes.
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int index = 0; index < argc; ++index) {
        // Native Windows argv is UTF-16. The OSAL converter preserves the text
        // without normalization or locale-dependent replacement.
        auto argument = vinc::osal::path_to_utf8(std::filesystem::path(argv[index]));
        if (!argument) {
            std::cerr << vinc::error_to_string(argument.error()) << '\n';
            return 2;
        }

        args.push_back(std::move(*argument));
    }

    return guarded_entry(args);
}
#else
int main(int argc, char** argv) {
    return guarded_entry(std::vector<std::string>(argv, argv + argc));
}
#endif

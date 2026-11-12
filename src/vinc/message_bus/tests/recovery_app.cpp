#include "../../../../tests/support/manager.hpp"
#include "../../param/tests/support.hpp"
#include <vinc/osal/notification.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <iostream>
#include <sstream>

namespace {
using namespace vinc;
using param_test::check;
using param_test::deadline;
using param_test::take;

constexpr char topic[] = "recovery/shared";

template<class Handle>
bool exists(Result<Handle> opened) {
    if (opened) {
        return true;
    }

    if (opened.error().is(osal::ErrorCode::NotFound)) {
        return false;
    }

    throw std::runtime_error(error_to_string(opened.error()));
}

int settings_command(ModuleManager& manager, MID writer, MID reader, const std::string& line) {
    std::istringstream input(line);
    std::string target;
    input >> target;
    std::vector<std::string> args;
    for (std::string word; input >> word;) {
        args.push_back(std::move(word));
    }

    if (target == "param") {
        take(detail::ManagerAccess::param_command(manager, std::move(args)));
        return 0;
    }

    check(target == "module" || target == "writer", "expected a Module or Param command");
    const auto owner = target == "writer" ? writer : reader;
    auto result = manager.command(owner, std::move(args), deadline());
    if (!result) {
        const Error full{message_bus::ErrorCode::ResourceExhausted, message_bus::ErrorOperation::CreateTopic};
        const auto causes = result.error().causes();
        if (!causes.empty() && causes.back() == full.record()) {
            return 1;
        }
    }
    return take(std::move(result));
}

void run(const char* name, const std::filesystem::path& plugin, message_bus::Limits limits, bool configure_params) {
    auto manager = take(detail::ManagerAccess::create(name, limits, test::host_path()));
    if (configure_params) {
        take(detail::ManagerAccess::configure_params(*manager, std::filesystem::current_path()));
    }
    ModulePolicy policy;
    policy.max_restarts = 0;
    take(manager->register_module_type({"messageBusTest", plugin, policy}));
    const auto writer = take(manager->start_module("messageBusTest", {"--name", "writer", "pub", topic, "2", "4096"}));
    take(manager->wait_running(writer, deadline()));
    const auto reader = take(manager->start_module("messageBusTest", {"--name", "reader", "sub", topic}));
    take(manager->wait_running(reader, deadline()));
    const auto publisher = manager->inspect(writer);
    const auto subscriber = manager->inspect(reader);
    check(publisher && subscriber && publisher->pid && subscriber->pid, "missing Host identity");
    std::cout << "READY " << publisher->pid << ' ' << subscriber->pid << std::endl;

    // The driver kills only the A central. Its Hosts use normal disconnect handling.
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "stop") {
            break;
        }

        int result;
        if (line.starts_with("push ")) {
            result = take(manager->command(writer, {"push", line.substr(5)}, deadline()));
        } else if (line == "last") {
            result = take(manager->command(reader, {"last"}, deadline()));
        } else if (line.starts_with("probe r")) {
            result = exists(osal::SharedMemory::open(line.substr(6), osal::SharedMemoryAccess::ReadOnly));
        } else if (line.starts_with("probe n")) {
            result = exists(osal::NotificationReceiver::open(line.substr(6)));
        } else if (configure_params) {
            result = settings_command(*manager, writer, reader, line);
        } else {
            throw std::runtime_error("unexpected test command: " + line);
        }
        std::cout << result << std::endl;
    }
    take(manager->wait_shutdown(deadline()));
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3 || argc == 4, "expected instance name, Plugin path and optional initial Topic limit");
        message_bus::Limits limits;
        if (argc == 4 && std::string_view(argv[3]) != "default") {
            limits.max_topics = std::stoul(argv[3]);
        }
        run(argv[1], argv[2], limits, argc == 4);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

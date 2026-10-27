#include "../../../../tests/support/manager.hpp"
#include <vinc/osal/path.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
constexpr auto operation_budget = std::chrono::seconds{15};
constexpr auto observe_interval = std::chrono::milliseconds{20};
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

void check(bool condition, const char* text) {
    if (!condition) {
        throw std::runtime_error(text);
    }
}

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

void take(vinc::Result<void> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
}

httplib::Headers authorize(httplib::Client& client) {
    const auto response = client.Get("/api/session", httplib::Headers{{"X-Vinc-UI", "1"}});
    check(response && response->status == 200, "browser capability bootstrap");
    const auto data = Json::parse(response->body);
    return {{"Authorization", "Bearer " + data.at("token").get<std::string>()}};
}

template<class Predicate> Json wait_json(httplib::Client& client, const httplib::Headers& headers,
                                       const std::string& path, Predicate predicate) {
    const auto deadline = Clock::now() + operation_budget;
    std::string last;
    do {
        const auto response = client.Get(path, headers);
        check(response && response->status == 200, "authorized topic request");
        auto data = Json::parse(response->body);
        last = response->body;
        if (predicate(data)) {
            return data;
        }
        std::this_thread::sleep_for(observe_interval);
    } while (Clock::now() < deadline);

    throw std::runtime_error("topic observation deadline: " + path + " last=" + last.substr(0, 4096));
}

template<class Predicate> Json wait_topic(httplib::Client& client, const httplib::Headers& headers,
                                         const std::string& name, Predicate predicate) {
    auto data = wait_json(client, headers, "/api/topics", [&](const auto& value) {
        for (const auto& topic : value.at("topics")) {
            if (topic.at("name") == name && predicate(topic)) {
                return true;
            }
        }
        return false;
    });
    for (const auto& topic : data.at("topics")) {
        if (topic.at("name") == name) {
            return topic;
        }
    }
    throw std::runtime_error("observed topic disappeared from local reply");
}

void graph(vinc::ModuleManager& manager, httplib::Client& client, const char* source) {
    auto unauthorized = client.Get("/api/graph");
    check(unauthorized && unauthorized->status == 401, "graph requires browser capability");
    const auto headers = authorize(client);
    take(manager.register_module_type({"uiGraphSource", std::filesystem::absolute(source), {}}));
    std::vector<vinc::MID> modules;
    constexpr unsigned isolated_modules = 9; // Force Module query pagination, not only Bus pagination.
    for (unsigned index = 0; index < isolated_modules; ++index) {
        const auto mid = take(manager.start_module("uiGraphSource", {"isolated" + std::to_string(index), "empty"}));
        take(manager.wait_running(mid, Clock::now() + operation_budget));
        modules.push_back(mid);
    }
    const auto source_mid = take(manager.start_module("uiGraphSource"));
    take(manager.wait_running(source_mid, Clock::now() + operation_budget));
    const auto response = client.Get("/api/graph", headers);
    check(response && response->status == 200, "complete real Module graph request");
    const auto snapshot = Json::parse(response->body);
    check(snapshot["apiVersion"] == 1 && snapshot["atomicSnapshot"] == false, "graph consistency contract");
    check(snapshot["modules"].size() >= isolated_modules + 2, "isolated Modules and multiple Module pages retained");
    unsigned publishers = 0;
    unsigned subscribers = 0;
    unsigned orphan = 0;
    for (const auto& connection : snapshot["connections"]) {
        if (connection["topic"] == "graph/shared") {
            check(connection["mid"] == std::to_string(source_mid), "authenticated owner MID");
            if (connection["role"] == "publish") {
                ++publishers;
            } else {
                ++subscribers;
            }
        } else if (connection["topic"] == "graph/noPublisher") {
            ++orphan;
        }
    }
    check(publishers == 2 && subscribers == 40 && orphan == 1, "all roles and registrations across same-topic pages");
    take(manager.stop_module(source_mid, Clock::now() + operation_budget));
    auto retired = wait_json(client, headers, "/api/graph", [](const auto& data) {
        for (const auto& connection : data.at("connections")) {
            if (connection.at("topic") == "graph/shared") {
                return false;
            }
        }
        return true;
    });
    bool retained = false;
    for (const auto& module : retired["modules"]) {
        if (module["mid"] == std::to_string(source_mid)) {
            retained = module["state"] == static_cast<unsigned>(vinc::ModuleState::Stopped);
        }
    }
    check(retained, "stopped Module record retained with no stale connections");
    for (const auto mid : modules) {
        take(manager.stop_module(mid, Clock::now() + operation_budget));
    }
    std::cout << "Graph: real Context, pagination, owner IDs, isolated Modules and retirement passed\n";
}

void monitor(vinc::ModuleManager& manager, httplib::Client& client, const char* source, const char* helper) {
    auto unauthorized = client.Get("/api/topics");
    check(unauthorized && unauthorized->status == 401, "diagnostics require capability");
    const auto headers = authorize(client);
    const auto duplicate = client.Get("/api/topics/preview?topic=a&topic=b", headers);
    check(duplicate && duplicate->status == 400, "ambiguous preview query rejected");
    const auto missing = client.Get("/api/topics/preview", headers);
    check(missing && missing->status == 400, "missing topic rejected");
    const auto embedded_null = client.Get("/api/topics/preview?topic=a%00b", headers);
    check(embedded_null && embedded_null->status == 400, "embedded NUL topic rejected");
    auto foreign_headers = headers;
    foreign_headers.emplace("Origin", "http://attacker.example");
    const auto foreign = client.Get("/api/topics", foreign_headers);
    check(foreign && foreign->status == 403, "bearer capability does not bypass origin checks");
    take(manager.register_module_type({"uiTopicSource", std::filesystem::absolute(source), {}}));
    const auto sender = take(manager.start_module("uiTopicSource"));
    take(manager.wait_running(sender, Clock::now() + operation_budget));
    check(take(manager.command(sender, {}, Clock::now() + operation_budget)) == 0, "publish known message");
    auto known = wait_topic(client, headers, "ui/preview", [](const auto& topic) {
        return topic.at("publishers").size() == 1;
    });
    check(known["type"] == "UserExampleFrame" && known["known_type"] == true, "compiled type resolved");
    check(known["publishers"][0]["mid"] == std::to_string(sender), "publisher MID preserved");
    const auto preview = wait_json(client, headers, "/api/topics/preview?topic=ui%2Fpreview", [](const auto& data) {
        return data.at("state") == "ready";
    });
    check(preview["publisher"] == "previewSource" && preview["text"].get<std::string>().find("sequence: 42") != std::string::npos,
        "real shared-memory message decoded and framework publisher preserved");
    check(preview["text"].get<std::string>().find("<script>plain text</script>") != std::string::npos,
        "message text returned as data");
    check(take(manager.command(sender, {}, Clock::now() + operation_budget)) == 0, "publish another sample");
    wait_topic(client, headers, "ui/preview", [](const auto& topic) {
        return topic.at("hz").is_number() && topic.at("hz").template get<double>() > 0;
    });

    take(manager.register_module_type({"messageBusTest", std::filesystem::absolute(helper), {}}));
    const auto writer = take(manager.start_module("messageBusTest", {"--name", "writer", "pub", "ui/collision", "2", "4096"}));
    const auto reader = take(manager.start_module("messageBusTest", {"--name", "reader", "sub", "ui/collision"}));
    take(manager.wait_running(writer, Clock::now() + operation_budget));
    take(manager.wait_running(reader, Clock::now() + operation_budget));
    for (const auto* value : {"1", "2", "3"}) {
        check(take(manager.command(writer, {"push", value}, Clock::now() + operation_budget)) == 0, "fill two-slot ring");
    }
    check(take(manager.command(reader, {"next"}, Clock::now() + operation_budget)) == 2, "slow reader skips overwritten record");
    auto lost = wait_topic(client, headers, "ui/collision", [](const auto& topic) {
        return topic.at("lost") == "1";
    });
    check(lost["known_type"] == false, "unknown schema still has diagnostics");
    const auto unknown = wait_json(client, headers, "/api/topics/preview?topic=ui%2Fcollision", [](const auto& value) {
        return value.at("state") == "unknown";
    });
    check(unknown["state"] == "unknown", "unknown type is not dynamically interpreted");
    check(take(manager.command(reader, {"pin"}, Clock::now() + operation_budget)) == 0, "pin real reader lease");
    check(take(manager.command(writer, {"push", "4"}, Clock::now() + operation_budget)) == 0, "write unprotected slot");
    auto blocked = std::async(std::launch::async, [&] {
        return manager.command(writer, {"push", "5"}, Clock::now() + operation_budget);
    });
    auto red = wait_topic(client, headers, "ui/collision", [](const auto& topic) {
        return topic.at("status") == "red";
    });
    check(red["publishers"][0]["collisions"] == "1", "collision event, not poll count");
    check(take(manager.command(reader, {"unpin"}, Clock::now() + operation_budget)) == 0, "release reader protection");
    check(take(blocked.get()) == 0, "writer completes after release");
    wait_topic(client, headers, "ui/collision", [](const auto& topic) {
        return topic.at("status") == "yellow";
    });
    wait_topic(client, headers, "ui/collision", [](const auto& topic) {
        return topic.at("status") == "green";
    });
    take(manager.stop_module(reader, Clock::now() + operation_budget));
    wait_topic(client, headers, "ui/collision", [](const auto& topic) {
        return topic.at("subscribers") == 0 && topic.at("lost") == "1";
    });
    take(manager.stop_module(writer, Clock::now() + operation_budget));
    take(manager.stop_module(sender, Clock::now() + operation_budget));
    std::cout << "Topic Monitor: real payload, Hz, shared loss, collision red/yellow/green and retirement passed\n";
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 6, "plugin, assets and topic fixture arguments");
        auto manager = take(vinc::test::create_manager());
        vinc::ModuleType type{"webUi", std::filesystem::absolute(argv[1]), {}};
        type.policy.stop_timeout = vinc::config::ui_stop_timeout;
        take(manager->register_module_type(type));
        check(manager->get_active_module_names().empty(), "registered UI does not auto-start");
        auto output = take(manager->output_hub().subscribe());
        const auto root = take(vinc::osal::path_to_utf8(std::filesystem::absolute(argv[2])));
        const auto mid = take(manager->start_module("webUi", {"--root", root, "--port", "0"}));
        take(manager->wait_started(mid));
        std::string trace;
        const auto deadline = std::chrono::steady_clock::now() + operation_budget;
        const std::string prefix = "http://127.0.0.1:";
        while (trace.find(prefix) == std::string::npos) {
            for (const auto& event : output.take().events) {
                trace += event->bytes;
            }

            check(std::chrono::steady_clock::now() < deadline, "UI startup output observed");
            std::this_thread::sleep_for(observe_interval);
        }

        const auto port = std::stoi(trace.substr(trace.find(prefix) + prefix.size()));
        httplib::Client client("127.0.0.1", port);
        client.set_read_timeout(operation_budget);
        const auto response = client.Get("/api/ui");
        check(response && response->status == 200, "ModuleHost serves HTTP");
        check(take(manager->command(mid, {"status"}, deadline)) == 0, "Shell hook works through pump");
        monitor(*manager, client, argv[3], argv[4]);
        graph(*manager, client, argv[5]);
        const auto old_headers = authorize(client);
        take(manager->stop_module(mid, Clock::now() + operation_budget));
        auto stopped = manager->inspect(mid);
        check(stopped && stopped->state == vinc::ModuleState::Stopped && !stopped->last_failure,
              "UI stopped through normal Module lifecycle");
        const auto restarted = take(manager->start_module("webUi", {"--root", root, "--port", std::to_string(port)}));
        take(manager->wait_started(restarted));
        const auto expired = client.Get("/api/topics", old_headers);
        check(expired && expired->status == 401, "UI restart invalidates old browser capability");
        take(manager->stop_module(restarted, std::chrono::steady_clock::now() + operation_budget));
        const auto failed = take(manager->start_module("webUi", {"--root", root, "--port", "invalid"}));
        check(!manager->wait_started(failed), "bad option is a Module startup error");
        manager->begin_shutdown();
        take(manager->wait_shutdown(std::chrono::steady_clock::now() + operation_budget));
        std::cout << "UI module tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

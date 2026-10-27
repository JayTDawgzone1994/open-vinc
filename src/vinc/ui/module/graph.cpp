#include <vinc/ui/detail/graph.hpp>
#include <vinc/module/context.hpp>
#include <nlohmann/json.hpp>
#include <map>
#include <set>

namespace vinc::ui::detail {
namespace {
Result<void> available(Context& context, Deadline deadline) {
    if (!context.living()) {
        return std::unexpected(Error{ErrorCode::Closing, ErrorOperation::Graph});
    }

    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(Error{ErrorCode::Timeout, ErrorOperation::Graph});
    }

    return {};
}
}

Result<std::string> collect_graph(Context& context, Deadline deadline) {
    using nlohmann::json;
    json modules = json::array();
    json connections = json::array();
    std::set<std::string> topics;
    MID after = 0;
    do {
        auto ready = available(context, deadline);
        if (!ready) {
            return std::unexpected(ready.error());
        }

        auto page = context.list_modules(after, deadline);
        if (!page) {
            return std::unexpected(wrap_error(ErrorCode::ModuleFailed, ErrorOperation::Graph, page.error()));
        }

        for (const auto& module : page->modules) {
            if (modules.size() >= config::ui_graph_modules) {
                return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Graph});
            }
            // Numeric identities travel as strings: browser numbers cannot represent 63-bit MIDs.
            modules.push_back({{"mid", std::to_string(module.mid)}, {"name", module.name},
                {"type", module.module_type}, {"state", static_cast<std::uint32_t>(module.state)}});
        }
        after = page->next;
    } while (after);

    message_bus::ConnectionCursor cursor;
    do {
        auto ready = available(context, deadline);
        if (!ready) {
            return std::unexpected(ready.error());
        }

        auto page = context.list_topic_connections(cursor, deadline);
        if (!page) {
            return std::unexpected(wrap_error(ErrorCode::BusFailed, ErrorOperation::Graph, page.error()));
        }

        for (const auto& connection : page->connections) {
            topics.insert(connection.key.topic);
            if (topics.size() > config::ui_graph_topics || connections.size() >= config::ui_graph_connections) {
                return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Graph});
            }
            connections.push_back({{"mid", std::to_string(connection.mid)}, {"topic", connection.key.topic},
                {"role", connection.key.role == message_bus::ConnectionRole::Publisher ? "publish" : "subscribe"},
                {"id", std::to_string(connection.key.id)}, {"schema", std::to_string(connection.schema)}});
        }
        cursor = std::move(page->next);
    } while (!cursor.topic.empty());

    auto ready = available(context, deadline);
    if (!ready) {
        return std::unexpected(ready.error());
    }

    const auto captured = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto text = json{{"apiVersion", 1}, {"capturedAt", captured}, {"atomicSnapshot", false},
        {"modules", std::move(modules)}, {"connections", std::move(connections)}}.dump();
    if (text.size() > config::ui_graph_response_bytes) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Graph});
    }

    ready = available(context, deadline);
    if (!ready) {
        return std::unexpected(ready.error());
    }

    return text;
}
}

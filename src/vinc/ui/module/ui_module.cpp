#include <vinc/module/base_module.hpp>
#include <vinc/ui/server.hpp>
#include <vinc/ui/detail/topic_monitor.hpp>
#include <vinc/ui/detail/graph.hpp>
#include <vinc/osal/path.hpp>
#include <vinc_ui_paths.hpp>
#include <charconv>
#include <iostream>
#include <thread>

namespace {
class UiModule final : public vinc::BaseModule<vinc::RunMode::Manual> {
public:
    UiModule() : BaseModule("ui") {
    }

    vinc::Result<std::string> on_name(std::vector<std::string>& args) override {
        // Shell forwards the command target; direct Manager callers may omit it.
        if (!args.empty() && args.front() == "webUi") {
            args.erase(args.begin());
        }

        return get_module_name();
    }

    vinc::Result<void> on_start(const std::vector<std::string>& args) override {
        vinc::ui::Options options;
        auto root = vinc::osal::path_from_utf8(vinc_ui_default_root);
        if (!root) {
            return std::unexpected(vinc::wrap_error(vinc::ui::ErrorCode::InvalidArgument,
                vinc::ui::ErrorOperation::Start, root.error()));
        }

        options.root = std::move(*root);
#ifdef VINC_COMPOSED_APP
        // The deployed Host lives beside the central executable. Resolve the
        // application's relative resource root without changing the user's cwd.
        auto executable = vinc::osal::executable_path();
        if (!executable) {
            return std::unexpected(vinc::wrap_error(vinc::ui::ErrorCode::FileFailed,
                vinc::ui::ErrorOperation::Start, executable.error()));
        }

        options.root = executable->parent_path() / options.root;
#endif
        for (std::size_t index = 0; index < args.size(); index += 2) {
            if (index + 1 >= args.size()) {
                return invalid_argument();
            }

            const auto& value = args[index + 1];
            if (args[index] == "--ip") {
                options.ip = value;
            } else if (args[index] == "--port") {
                unsigned port = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port > 65535) {
                    return invalid_argument();
                }

                options.port = static_cast<std::uint16_t>(port);
            } else if (args[index] == "--root") {
                auto path = vinc::osal::path_from_utf8(value);
                if (!path) {
                    return std::unexpected(vinc::wrap_error(vinc::ui::ErrorCode::InvalidArgument,
                        vinc::ui::ErrorOperation::Start, path.error()));
                }

                options.root = std::move(*path);
            } else {
                return invalid_argument();
            }
        }

        const auto ip = options.ip;
        monitor_ = std::make_shared<vinc::ui::detail::TopicMonitor>(context);
        options.read_topics = [monitor = monitor_](const std::string& topic) {
            return monitor->read(topic);
        };
        graph_ = std::make_shared<vinc::ui::detail::GraphRequests>([this](vinc::Deadline deadline) {
            return vinc::ui::detail::collect_graph(context, deadline);
        });
        options.read_graph = [graph = graph_] { return graph->request(); };
        auto started = vinc::ui::Server::start(std::move(options));
        if (!started) {
            return std::unexpected(started.error());
        }

        server_ = std::move(*started);
        const auto host = ip.find(':') == ip.npos ? ip : '[' + ip + ']';
        std::cout << "Web UI listening on http://" << host << ':' << server_->port() << "/\n" << std::flush;
        return {};
    }

    vinc::Result<void> main() override {
        while (context.living()) {
            auto pumped = context.pumpOnce();
            if (!pumped) {
                return std::unexpected(pumped.error());
            }

            graph_->process();
            monitor_->poll();
            std::this_thread::sleep_for(vinc::config::ui_monitor_poll_interval);
        }
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args.front() == get_module_name()) {
            args.erase(args.begin());
        }

        if (args.size() != 1 || args[0] != "status") {
            return std::unexpected(vinc::Error{vinc::ui::ErrorCode::InvalidArgument,
                                              vinc::ui::ErrorOperation::Command});
        }

        std::cout << "UI running=" << server_->running() << " port=" << server_->port() << '\n' << std::flush;
        return 0;
    }

    vinc::Result<void> on_stop() override {
        if (graph_) {
            graph_->stop();
        }
        server_.reset();
        graph_.reset();
        monitor_.reset();
        return {};
    }

private:
    vinc::Result<void> invalid_argument() {
        return std::unexpected(vinc::Error{vinc::ui::ErrorCode::InvalidArgument,
                                          vinc::ui::ErrorOperation::Start});
    }
    std::unique_ptr<vinc::ui::Server> server_;
    std::shared_ptr<vinc::ui::detail::TopicMonitor> monitor_;
    std::shared_ptr<vinc::ui::detail::GraphRequests> graph_;
};
}
REGISTER_MODULE(UiModule, "webUi")

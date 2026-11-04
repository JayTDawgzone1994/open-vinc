#include <vinc/module/base_module.hpp>
#include <vinc/ui/server.hpp>
#include <vinc/ui/detail/topic_monitor.hpp>
#include <vinc/ui/detail/graph.hpp>
#include <vinc/osal/path.hpp>
#include <vinc_ui_paths.hpp>
#include <charconv>
#include <iostream>
#include <optional>
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
        auto& options = options_;
        std::optional<std::uint16_t> port_override;
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
            if (args[index] == "--enabled" && (value == "true" || value == "false")) {
                enabled_ = value == "true";
            } else if (args[index] == "--ip") {
                options.ip = value;
            } else if (args[index] == "--port") {
                unsigned port = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port > 65535) {
                    return invalid_argument();
                }

                port_override = static_cast<std::uint16_t>(port);
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

        auto parameters = initialize_port(port_override);
        if (!parameters) {
            return parameters;
        }

        if (!enabled_) {
            return {};
        }
        return start_server();
    }

    vinc::Result<void> start_server() {
        auto configured = read_port();
        if (!configured) {
            return std::unexpected(configured.error());
        }

        options_.port = *configured;
        // Each enable takes a fresh asset/plugin snapshot. Context work stays on
        // this Module's pump thread; the HTTP workers only access bounded caches.
        auto options = options_;
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
            if (enabled_ && (!server_ || !server_->running())) {
                return std::unexpected(start_failure_.value_or(vinc::Error{
                    vinc::ui::ErrorCode::WorkerFailed, vinc::ui::ErrorOperation::Start}));
            }
            if (graph_) {
                graph_->process();
            }
            if (monitor_) {
                monitor_->poll();
            }
            std::this_thread::sleep_for(vinc::config::ui_monitor_poll_interval);
        }
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (!args.empty() && args.front() == get_module_name()) {
            args.erase(args.begin());
        }

        if (args.size() != 1) {
            return std::unexpected(vinc::Error{vinc::ui::ErrorCode::InvalidArgument,
                                              vinc::ui::ErrorOperation::Command});
        }
        if (args[0] == "disable") {
            enabled_ = false;
            stop_server();
        } else if (args[0] == "enable") {
            enabled_ = true;
            if (!server_) {
                auto started = start_server();
                if (!started) {
                    start_failure_ = started.error();
                    return std::unexpected(started.error());
                }
                start_failure_.reset();
            }
        } else if (args[0] != "status") {
            return std::unexpected(vinc::Error{vinc::ui::ErrorCode::InvalidArgument,
                                              vinc::ui::ErrorOperation::Command});
        }

        auto configured = read_port();
        if (!configured) {
            return std::unexpected(configured.error());
        }

        std::cout << "UI running=" << (server_ && server_->running())
                  << " port=" << (server_ ? server_->port() : options_.port)
                  << " configured_port=" << *configured
                  << " enabled=" << enabled_ << '\n' << std::flush;
        return 0;
    }

    vinc::Result<void> on_stop() override {
        stop_server();
        return {};
    }

private:
    vinc::Error parameter_failure(const vinc::Error& cause) const {
        return vinc::wrap_error(vinc::ui::ErrorCode::ParamFailed,
                               vinc::ui::ErrorOperation::ConfigurePort, cause);
    }

    vinc::Result<void> initialize_port(std::optional<std::uint16_t> override) {
        auto registered = context.param.register_param<std::uint16_t>(
            "port", vinc::config::ui_port, {.restart_hint = vinc::param::RestartHint::Module});
        if (!registered) {
            return std::unexpected(parameter_failure(registered.error()));
        }

        if (override) {
            auto changed = context.param.set("port", *override);
            if (!changed) {
                return std::unexpected(parameter_failure(changed.error()));
            }
        }

        auto current = read_port();
        if (!current) {
            return std::unexpected(current.error());
        }

        auto watched = context.param.on_change([this](const vinc::param::ParamMap& values) {
            const auto found = values.find(get_module_name() + ".port");
            if (found == values.end()) {
                return;
            }

            auto accepted = accept_port(found->second);
            if (!accepted) {
                std::cerr << "UI parameter rejected: " << vinc::error_to_string(accepted.error()) << '\n';
            }
            // A valid update is desired configuration, not permission to stop
            // the active server or invalidate browser sessions.
        });
        if (!watched) {
            return std::unexpected(parameter_failure(watched.error()));
        }

        return {};
    }

    vinc::Result<std::uint16_t> accept_port(const vinc::param::Entry& entry) {
        // The declared uint16 type bounds ports; zero explicitly requests an
        // OS-selected endpoint. Resource/bind failures must not rewrite Param.
        auto port = entry.get<std::uint16_t>();
        if (!port) {
            auto restored = context.param.set("port", accepted_port_);
            if (!restored) {
                return std::unexpected(parameter_failure(restored.error()));
            }

            return std::unexpected(parameter_failure(port.error()));
        }

        accepted_port_ = *port;
        return accepted_port_;
    }

    vinc::Result<std::uint16_t> read_port() {
        // Registration/set acknowledge central commit, not local pump cache.
        auto current = context.param.read_current("port");
        if (!current) {
            return std::unexpected(parameter_failure(current.error()));
        }

        return accept_port(*current);
    }

    void stop_server() {
        if (graph_) {
            graph_->stop();
        }
        server_.reset();
        graph_.reset();
        monitor_.reset();
    }
    vinc::Result<void> invalid_argument() {
        return std::unexpected(vinc::Error{vinc::ui::ErrorCode::InvalidArgument,
                                          vinc::ui::ErrorOperation::Start});
    }
    std::unique_ptr<vinc::ui::Server> server_;
    vinc::ui::Options options_;
    std::uint16_t accepted_port_ = vinc::config::ui_port;
    bool enabled_ = true;
    std::optional<vinc::Error> start_failure_;
    std::shared_ptr<vinc::ui::detail::TopicMonitor> monitor_;
    std::shared_ptr<vinc::ui::detail::GraphRequests> graph_;
};
}
REGISTER_MODULE(UiModule, "webUi")

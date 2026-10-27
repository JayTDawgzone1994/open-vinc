#include <vinc/ui/server.hpp>
#include <vinc/ui/detail/assets.hpp>
#include <vinc/osal/socket_address.hpp>
#include <vinc/osal/random.hpp>
#include <httplib.h>
#include <atomic>
#include <array>
#include <thread>

namespace vinc::ui {
static_assert(config::ui_session_token_bytes >= 16 && config::ui_session_token_bytes <= 64);
namespace {
std::string authority(std::string ip, int port) {
    if (ip.find(':') != ip.npos) {
        ip = '[' + ip + ']';
    }

    if (port != 80) {
        ip += ':' + std::to_string(port);
    }

    return ip;
}

bool allowed_request(const httplib::Request& request) {
    if (request.get_header_value_count("Host") != 1) {
        return false;
    }

    const auto host = request.get_header_value("Host");
    const auto local = authority(request.local_addr, request.local_port);
    const bool loopback = request.local_addr == "127.0.0.1" || request.local_addr == "::1";
    if (host != local && !(loopback && host == authority("localhost", request.local_port))) {
        return false;
    }

    // Reject foreign browser origins and ambiguous duplicate origin headers.
    // This is not user authentication or authority to mutate central state.
    if (request.get_header_value_count("Origin") > 1) {
        return false;
    }
    if (request.has_header("Origin") && request.get_header_value("Origin") != "http://" + host) {
        return false;
    }

    return request.get_header_value("Sec-Fetch-Site") != "cross-site";
}
}

struct Server::Impl {
    detail::Assets assets;
    httplib::Server http;
    std::thread listener;
    std::atomic<bool> exited{false};
    std::uint16_t bound_port = 0;
    std::string session_token;
    std::function<Result<std::string>(const std::string&)> read_topics;
    std::function<Result<std::string>()> read_graph;

    ~Impl() {
        stop();
    }

    void stop() {
        http.stop();
        if (listener.joinable()) {
            listener.join();
        }
    }

    void configure_routes() {
        http.new_task_queue = [] {
            return new httplib::ThreadPool(config::ui_http_workers, config::ui_http_workers,
                                           config::ui_http_queue);
        };
        http.set_read_timeout(config::ui_http_timeout);
        http.set_write_timeout(config::ui_http_timeout);
        http.set_keep_alive_max_count(1);
        http.set_payload_max_length(0);
        http.set_default_headers({
            {"X-Content-Type-Options", "nosniff"},
            {"Referrer-Policy", "no-referrer"},
            {"Cache-Control", "no-store"},
            {"Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'"}
        });
        http.set_exception_handler([](const auto&, auto& response, std::exception_ptr) {
            // Third-party/STL exceptions stay at the HTTP worker boundary. No
            // framework expected failure is implemented by throwing an exception.
            response.status = 500;
            response.set_content("UI request failed", "text/plain");
        });
        http.set_pre_routing_handler([](const auto& request, auto& response) {
            if (!allowed_request(request)) {
                response.status = 403;
                return httplib::Server::HandlerResponse::Handled;
            }

            if (request.method != "GET" && request.method != "HEAD") {
                response.status = 405;
                return httplib::Server::HandlerResponse::Handled;
            }

            return httplib::Server::HandlerResponse::Unhandled;
        });
        http.Get("/api/ui", [this](const auto&, auto& response) {
            response.set_content(assets.catalog, "application/json");
        });
        http.Get("/api/session", [this](const auto& request, auto& response) {
            // A custom header cannot be supplied by cross-site forms/navigation;
            // foreign fetch preflights are refused by the existing method/origin gate.
            if (request.get_header_value_count("X-Vinc-UI") != 1 || request.get_header_value("X-Vinc-UI") != "1") {
                response.status = 403;
                return;
            }

            response.set_content("{\"token\":\"" + session_token + "\"}", "application/json");
        });
        http.Get("/api/graph", [this](const auto& request, auto& response) {
            if (!authorized(request)) {
                response.status = 401;
                return;
            }

            if (!request.params.empty()) {
                response.status = 400;
                return;
            }

            if (!read_graph) {
                response.status = 503;
                return;
            }

            if (request.method == "HEAD") {
                response.status = 200;
                return;
            }

            auto data = read_graph();
            if (!data) {
                response.status = 503;
                if (data.error().is(ErrorCode::Busy)) {
                    response.status = 429;
                } else if (data.error().is(ErrorCode::Timeout)) {
                    response.status = 504;
                }
                response.set_content("Graph refresh failed: " + error_to_string(data.error()), "text/plain");
                return;
            }

            response.set_content(std::move(*data), "application/json");
        });
        http.Get("/api/topics", [this](const auto& request, auto& response) {
            topic_request(request, response, false);
        });
        http.Get("/api/topics/preview", [this](const auto& request, auto& response) {
            topic_request(request, response, true);
        });
        http.Get(R"(/.*)", [this](const auto& request, auto& response) {
            const auto path = request.path == "/" ? "/index.html" : request.path;
            const auto found = assets.routes.find(path);
            if (found == assets.routes.end()) {
                response.status = 404;
                return;
            }

            response.set_content(found->second.bytes, found->second.mime);
        });
    }

    bool authorized(const httplib::Request& request) const {
        if (request.get_header_value_count("Authorization") != 1) {
            return false;
        }

        const auto supplied = request.get_header_value("Authorization");
        const auto expected = "Bearer " + session_token;
        if (supplied.size() != expected.size()) {
            return false;
        }

        unsigned difference = 0;
        for (std::size_t index = 0; index < expected.size(); ++index) {
            difference |= static_cast<unsigned char>(supplied[index]) ^ static_cast<unsigned char>(expected[index]);
        }
        return difference == 0;
    }

    void topic_request(const httplib::Request& request, httplib::Response& response, bool preview) {
        if (!authorized(request)) {
            response.status = 401;
            return;
        }
        if (!read_topics) {
            response.status = 503;
            return;
        }

        std::string topic;
        if (preview) {
            topic = request.get_param_value("topic");
            if (request.params.size() != 1 || request.get_param_value_count("topic") != 1 ||
                topic.empty() || topic.size() > config::ui_topic_name_bytes ||
                topic.find('\0') != topic.npos || topic.find('.') != topic.npos) {
                response.status = 400;
                return;
            }
        } else if (!request.params.empty()) {
            response.status = 400;
            return;
        }

        auto data = read_topics(topic);
        if (!data) {
            response.status = 503;
            response.set_content("Topic diagnostics unavailable", "text/plain");
            return;
        }

        response.set_content(std::move(*data), "application/json");
    }

    Result<void> initialize_session() {
        std::array<std::byte, config::ui_session_token_bytes> bytes;
        auto random = osal::random_bytes(bytes);
        if (!random) {
            return std::unexpected(wrap_error(ErrorCode::SessionFailed, ErrorOperation::Session, random.error()));
        }

        constexpr char hex[] = "0123456789abcdef";
        for (const auto byte : bytes) {
            const auto value = std::to_integer<unsigned>(byte);
            session_token += hex[value >> 4];
            session_token += hex[value & 15];
        }
        return {};
    }

    Result<void> bind(const Options& options) {
        bool socket_options_ok = true;
        http.set_socket_options([&socket_options_ok](auto socket) {
            // Override upstream's reuse-port default. Windows requires exclusive
            // binding; POSIX reuse-address permits restart without sharing a listener.
#ifdef _WIN32
            const auto option = SO_EXCLUSIVEADDRUSE;
#else
            const auto option = SO_REUSEADDR;
#endif
            socket_options_ok = httplib::set_socket_opt(socket, SOL_SOCKET, option, 1) && socket_options_ok;
        });
        int port = options.port;
        if (port == 0) {
            port = http.bind_to_any_port(options.ip);
        } else if (!http.bind_to_port(options.ip, port)) {
            port = -1;
        }

        // Socket options run synchronously during bind, never during accept.
        http.set_socket_options(nullptr);
        if (port <= 0 || !socket_options_ok) {
            http.stop();
            // cpp-httplib does not expose a stable native bind error here.
            return std::unexpected(Error{ErrorCode::BindFailed, ErrorOperation::Start});
        }

        bound_port = static_cast<std::uint16_t>(port);
        return {};
    }

    Result<void> launch() {
        listener = std::thread([this] {
            try {
                http.listen_after_bind();
            } catch (...) {
                // Infrastructure allocation/thread failures stop this UI service.
                // Heartbeat ownership remains with ModuleHost.
            }
            exited = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + config::ui_start_timeout;
        while (!http.is_running() && !exited && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(config::ui_start_poll_interval);
        }

        if (!http.is_running()) {
            stop();
            return std::unexpected(Error{ErrorCode::WorkerFailed, ErrorOperation::Start});
        }

        return {};
    }
};

Server::Server(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
Server::~Server() = default;

Result<std::unique_ptr<Server>> Server::start(Options options) {
    if (options.ip == "localhost") {
        options.ip = "127.0.0.1";
    }

    if (options.root.empty() ||
        (!osal::IPv4Address::parse(options.ip) && !osal::IPv6Address::parse(options.ip))) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Start});
    }

    auto assets = detail::load_assets(options.root);
    if (!assets) {
        return std::unexpected(wrap_error(ErrorCode::InvalidPackage, ErrorOperation::Start, assets.error()));
    }

    auto impl = std::make_unique<Impl>();
    impl->assets = std::move(*assets);
    impl->read_topics = std::move(options.read_topics);
    impl->read_graph = std::move(options.read_graph);
    auto session = impl->initialize_session();
    if (!session) {
        return std::unexpected(session.error());
    }
    impl->configure_routes();
    auto bound = impl->bind(options);
    if (!bound) {
        return std::unexpected(bound.error());
    }

    auto launched = impl->launch();
    if (!launched) {
        return std::unexpected(launched.error());
    }

    return std::unique_ptr<Server>(new Server(std::move(impl)));
}

std::uint16_t Server::port() const noexcept {
    return impl_->bound_port;
}
bool Server::running() const noexcept {
    return impl_->http.is_running() && !impl_->exited;
}
void Server::stop() {
    impl_->stop();
}
}

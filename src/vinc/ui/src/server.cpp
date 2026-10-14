#include <vinc/ui/server.hpp>
#include <vinc/ui/detail/assets.hpp>
#include <vinc/osal/socket_address.hpp>
#include <httplib.h>
#include <atomic>
#include <thread>

namespace vinc::ui {
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

    // This read-only shell has no admin API or login session. Reject foreign
    // browser origins now; central authority still needs its own future design.
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

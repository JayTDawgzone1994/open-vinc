#pragma once
#include <vinc/ui/error.hpp>
#include <filesystem>
#include <memory>
#include <functional>
#include <string>

namespace vinc::ui {
struct Options {
    std::filesystem::path root;
    std::string ip = "127.0.0.1";
    std::uint16_t port = config::ui_port;
    // Framework integration only: HTTP workers invoke a bounded local-cache reader,
    // never Context or application hooks. Empty topic requests the diagnostic list.
    std::function<Result<std::string>(const std::string& topic)> read_topics;
    // Enqueue one bounded job and wait; Context access stays on the Module main thread.
    std::function<Result<std::string>()> read_graph;
};

// Owns immutable assets and HTTP workers. No central authority is exposed in v1.
// The owner serializes start/stop/destruction; running() is a concurrent query.
class Server {
public:
    static Result<std::unique_ptr<Server>> start(Options options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    std::uint16_t port() const noexcept;
    bool running() const noexcept;
    void stop();
private:
    struct Impl;
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}

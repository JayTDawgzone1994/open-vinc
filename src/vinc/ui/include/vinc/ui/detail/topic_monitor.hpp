#pragma once

// Framework integration: only the module main thread calls poll(). HTTP workers
// call read() against bounded local caches; they never invoke Context or callbacks.
#include <vinc/module/context.hpp>
#include <memory>
#include <string>

namespace vinc::ui::detail {
class TopicMonitor {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit TopicMonitor(Context& context);
    ~TopicMonitor();
    void poll();
    Result<std::string> read(const std::string& topic);
};
}

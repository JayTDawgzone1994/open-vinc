#pragma once

// Application extension point inside a child process.
// Users implement hooks; the framework hides Socket commands, heartbeat and Bus attachment.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <vinc/bus/bus.hpp>

namespace vinc {
namespace detail { class ModuleBusAccess; }
class BaseModule {
    friend class detail::ModuleBusAccess;
public:
    // The framework supplies the centrally allocated, final name.
    explicit BaseModule(std::string node_name);
    virtual ~BaseModule();
    const std::string& get_node_name() const noexcept { return node_name_; }
    // All application hooks execute serially on the module's main thread.
    virtual int32_t on_start(const std::vector<std::string>&) { return 0; }
    virtual void on_stop() {}
    virtual int32_t node_interface(std::vector<std::string> args) = 0;
    virtual std::vector<std::string> node_autoComplete(std::vector<std::string>) { return {}; }
protected:
    // Available from on_start through on_stop; constructors run before attachment.
    bus::Client& bus();
private:
    const std::string node_name_;
    std::shared_ptr<bus::Client> bus_;
};
namespace detail {
using ModuleFactory = std::function<std::unique_ptr<BaseModule>(std::string)>;
bool register_compiled_module(std::string, ModuleFactory);
}
int run_module_process(const std::vector<std::string>& arguments);
}

#define VINC_CONCAT_INTERNAL(a, b) a##b
#define VINC_CONCAT(a, b) VINC_CONCAT_INTERNAL(a, b)
// Registration happens only in the module executable. No user factory runs in the central process.
#define REGISTER_MODULE(ClassType, ModuleName) \
    namespace { const bool VINC_CONCAT(vinc_registered_, __LINE__) = \
        ::vinc::detail::register_compiled_module(ModuleName, [](std::string name) { \
            return std::make_unique<ClassType>(std::move(name)); \
        }); }

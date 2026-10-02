#pragma once

// Application extension point inside a child process.
// Users implement hooks; the framework hides Socket commands, heartbeat and Bus attachment.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <vinc/bus/bus.hpp>
#include <module/mid.hpp>

namespace vinc {
namespace detail { class ModuleBusAccess; }
class BaseModule {
    friend class detail::ModuleBusAccess;
public:
    // The framework supplies a default proposal; on_name runs before central registration.
    explicit BaseModule(std::string module_name);
    virtual ~BaseModule();
    const std::string& get_module_name() const noexcept { return module_name_; }
    MID get_mid() const noexcept { return mid_; }
    // Called once per process before name registration and on_start. The initial
    // name is only a proposal. Parse application args here and return the desired
    // final name; do not start normal work or use Bus before central acceptance.
    // Consumed arguments may be erased here; on_start receives the remaining args.
    virtual std::string on_name(std::vector<std::string>&) { return module_name_; }
    // All application hooks execute serially on the module's main thread.
    virtual int32_t on_start(const std::vector<std::string>&) { return 0; }
    virtual void on_stop() {}
    virtual int32_t module_interface(std::vector<std::string> args) = 0;
    virtual std::vector<std::string> module_autoComplete(std::vector<std::string>) { return {}; }
protected:
    // Available from on_start through on_stop; constructors run before attachment.
    bus::Client& bus();
private:
    std::string module_name_; // Runtime may replace it once, after registration approval.
    MID mid_ = 0; // Set after construction, before on_name.
    std::shared_ptr<bus::Client> bus_;
};
namespace detail {
using ModuleFactory = std::function<std::unique_ptr<BaseModule>(std::string)>;
bool register_compiled_module_type(std::string, ModuleFactory);
}
int run_module_process(const std::vector<std::string>& arguments);
}

#define VINC_CONCAT_INTERNAL(a, b) a##b
#define VINC_CONCAT(a, b) VINC_CONCAT_INTERNAL(a, b)
// Registration happens only in the module executable. No user factory runs in the central process.
#define REGISTER_MODULE(ClassType, ModuleTypeName) \
    namespace { const bool VINC_CONCAT(vinc_registered_, __LINE__) = \
        ::vinc::detail::register_compiled_module_type(ModuleTypeName, [](std::string name) { \
            return std::make_unique<ClassType>(std::move(name)); \
        }); }

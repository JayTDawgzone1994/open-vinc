#pragma once
#include <vinc/module/context.hpp>
#include <vinc/module/mid.hpp>

// Shared Host-facing vtable for both SDK run modes. Plugin owns create/destroy.
namespace vinc::detail {
class ModuleMessageBusAccess;
class ModuleObject {
    friend class detail::ModuleMessageBusAccess;

public:
    // The framework supplies a default proposal; on_name runs before central registration.
    explicit ModuleObject(std::string module_name);
    virtual ~ModuleObject();

    const std::string& get_module_name() const noexcept {
        return module_name_;
    }

    MID get_mid() const noexcept {
        return mid_;
    }

    // Called once per process before name registration and on_start. The initial
    // name is only a proposal. Parse application args here and return the desired
    // final name; do not start normal work or use Message Bus before central acceptance.
    // Consumed arguments may be erased here; on_start receives the remaining args.
    virtual Result<std::string> on_name(std::vector<std::string>&) {
        return module_name_;
    }

    // Lifecycle hooks run on the main thread; command hooks run on the pump caller.
    virtual Result<void> on_start(const std::vector<std::string>&) {
        return {};
    }

    virtual Result<void> main() = 0;

    virtual Result<void> on_stop() {
        return {};
    }

    // The integer is an application command result, independent of framework failure.
    virtual Result<std::int32_t> module_interface(std::vector<std::string> args) = 0;

    virtual Result<std::vector<std::string>> module_autoComplete(std::vector<std::string>) {
        return {};
    }

protected:
    Context context;

private:
    std::string module_name_; // Runtime may replace it once, after registration approval.
    MID mid_ = 0;             // Set after construction, before on_name.

};
}


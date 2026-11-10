// Keep the real attachment implementation in its own translation unit, so the
// Recorder test does not copy private Context layout or mix component namespaces.
#include "../../module_system/src/runtime/module_runtime.cpp"

namespace vinc::bag::test {
Result<void> run(std::unique_ptr<vinc::detail::ModuleObject> module, vinc::detail::ContextRuntime& runtime) {
    vinc::detail::ModuleLocks locks{{}, 1};
    vinc::detail::ModuleParams params{{}, locks};
    rpc::detail::Host rpc{nullptr};
    // Destroy the Module before its borrowed Host services, also on assertions.
    struct ModuleCleanup {
        std::unique_ptr<vinc::detail::ModuleObject>& module;
        ~ModuleCleanup() {
            module.reset();
        }
    } cleanup{module};
    vinc::detail::ModuleMessageBusAccess::attach(*module, {}, runtime, params, nullptr, rpc);
    auto result = module->main();
    auto stopped = module->on_stop();
    if (!result) {
        return result;
    }

    return stopped;
}
}

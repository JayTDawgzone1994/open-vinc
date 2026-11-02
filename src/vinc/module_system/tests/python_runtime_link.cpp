#include <string>
#include <vinc/error.hpp>
#include <vinc/module/detail/module_runtime.hpp>

// Force the real Host runtime and generated catalog into a static-CRT image.
// Invalid bootstrap must be rejected before any Module or central is started.
int main() {
#if defined(_DLL)
    return 1;
#endif
    constexpr int invalid_bootstrap_exit = 64;
    if (vinc::run_module_process({}) != invalid_bootstrap_exit) {
        return 2;
    }

    const auto text = vinc::error_to_string(vinc::Error{});
    if (text.empty()) {
        return 3;
    }
    return 0;
}

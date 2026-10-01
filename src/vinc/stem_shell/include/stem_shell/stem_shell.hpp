#pragma once

// Central user-facing Shell with asynchronous commands and a foreground Tcl script.
// Its exit path waits for module processes and Bus cleanup before returning.

#include <module/module_manager.hpp>
#include <filesystem>
#include <iosfwd>
#include <memory>

namespace vinc {
class StemShell {
public:
    StemShell();
    explicit StemShell(ModuleManager&, std::ostream&);
    ~StemShell();
    StemShell(const StemShell&) = delete;
    StemShell& operator=(const StemShell&) = delete;
    void run(std::filesystem::path startup_script = {}, std::vector<std::string> script_args = {});
    // Synchronous command entry. Does not parse Tcl.
    int8_t process_line(const std::string&);
    std::string get_last_error() const;
    void request_shutdown();
    void wait_shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

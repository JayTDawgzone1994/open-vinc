#pragma once
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
    void run();
    // Synchronous compatibility entry for the existing script interpreter.
    int8_t process_line(const std::string&);
    int8_t run_script(std::filesystem::path);
    std::string get_last_error() const;
    void request_shutdown();
    void wait_shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

#pragma once
#include <vinc/log/types.hpp>
#include <vinc/control/channel.hpp>
#include <vinc/output_hub/output_hub.hpp>
#include <filesystem>

namespace vinc::log::detail {
class Manager {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Manager(std::unique_ptr<Impl>);

public:
    static Result<std::unique_ptr<Manager>> create(control::Service&, output_hub::OutputHub&,
                                                   std::string instance);
    ~Manager();
    Result<void> configure(std::filesystem::path root, FileOptions options = {});
    Result<void> listen(std::uint64_t mid, std::string endpoint, control::Options options);
    void identify(std::uint64_t mid, std::string name, std::uint64_t pid);
    void exited(std::uint64_t mid);
    void diagnostic(std::string text, Level level = Level::Warn);
    Result<std::string> command(const std::vector<std::string>& args);
    std::string take(View& view);
    std::string notification(std::uint64_t& cursor);
};

Result<std::string> view_command(View& view, const std::vector<std::string>& args);
}

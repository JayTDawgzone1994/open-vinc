#pragma once
#include <vinc/log/types.hpp>
#include <filesystem>
#include <memory>

namespace vinc::log::detail {
// Sole owner is the disk worker. No other thread opens, scans or removes log files.
class Files {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    Files();
    ~Files();
    Result<void> configure(std::filesystem::path root, FileOptions options);
    Result<void> append(std::string_view day, std::string_view encoded);
    Result<void> flush();
    std::uint64_t bytes() const;
};
}

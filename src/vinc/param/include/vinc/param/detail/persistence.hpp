#pragma once
#include <vinc/param/types.hpp>
#include <filesystem>

namespace vinc::param::detail {
struct SavedEntry {
    Value default_value;
    Value value;
};

using SavedMap = std::map<std::string, SavedEntry, std::less<>>;

// Central worker only. The saved map is deliberately separate from live values.
class Persistence {
public:
    Result<SavedMap> load(std::filesystem::path file);
    Result<void> write(const SavedMap& entries);

    bool enabled() const noexcept {
        return enabled_;
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
    bool enabled_ = false;
};
}

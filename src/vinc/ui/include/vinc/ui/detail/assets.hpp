#pragma once
#include <vinc/ui/error.hpp>
#include <filesystem>
#include <map>
#include <string>
namespace vinc::ui::detail {
struct Asset {
    std::string bytes;
    std::string mime;
};
struct Assets {
    std::map<std::string, Asset, std::less<>> routes;
    std::string catalog;
};
// Loads bounded files once. Serving never reopens a mutable deployment path.
Result<Assets> load_assets(const std::filesystem::path& root);
}

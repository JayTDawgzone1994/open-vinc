#include <vinc/ui/detail/assets.hpp>
#include <vinc/osal/path.hpp>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <vinc_ui_official.hpp>
#include <algorithm>
#include <fstream>
#include <set>

namespace vinc::ui::detail {
namespace {
using Json = nlohmann::json;
using Files = std::map<std::string, std::string>;

Error failure(ErrorCode code) {
    return Error{code, ErrorOperation::Load};
}

Error file_failure(const std::error_code& error) {
#ifdef _WIN32
    const auto source = NativeErrorSource::WindowsGeneric;
#elif defined(__APPLE__)
    const auto source = NativeErrorSource::DarwinGeneric;
#else
    const auto source = NativeErrorSource::LinuxGeneric;
#endif
    // filesystem error_code may use the platform system category. Only use the
    // generic namespace after asking that category for its portable condition.
    return Error{ErrorCode::FileFailed, ErrorOperation::Load,
                 {source, error.default_error_condition().value()}};
}

bool safe_path(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.find("..") != name.npos) {
        return false;
    }

    return std::all_of(name.begin(), name.end(), [](unsigned char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') || value == '-' || value == '_' ||
               value == '.' || value == '/';
    });
}

bool identifier(std::string_view text) {
    return !text.empty() && text.size() <= config::ui_identifier_bytes && safe_path(text) && text.find('/') == text.npos;
}

Result<void> check_directory(const std::filesystem::path& path) {
    // Refuse symlink ancestors as well as symlink entries inside the package.
    auto current = path;
    while (!current.empty()) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error) {
            return std::unexpected(file_failure(error));
        }

        if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
            return std::unexpected(failure(ErrorCode::InvalidPackage));
        }

        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }

        current = parent;
    }

    return {};
}

Result<std::string> read_file(const std::filesystem::path& path, std::size_t& budget) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected(file_failure(error));
    }

    if (size > config::ui_file_bytes || size > budget) {
        return std::unexpected(failure(ErrorCode::CapacityExceeded));
    }

    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(size)) ||
        input.peek() != std::char_traits<char>::eof() || input.bad()) {
        return std::unexpected(failure(ErrorCode::FileFailed));
    }

    budget -= bytes.size();
    return bytes;
}

Result<Files> read_package(const std::filesystem::path& root, std::size_t& budget) {
    auto checked = check_directory(root);
    if (!checked) {
        return std::unexpected(checked.error());
    }

    Files files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator entry(root, error);
    const std::filesystem::recursive_directory_iterator end;
    std::size_t entries = 0;
    while (!error && entry != end) {
        if (++entries > config::ui_package_entries) {
            return std::unexpected(failure(ErrorCode::CapacityExceeded));
        }

        const auto status = entry->symlink_status(error);
        if (error) {
            break;
        }

        if (std::filesystem::is_symlink(status)) {
            return std::unexpected(failure(ErrorCode::InvalidPackage));
        }

        if (std::filesystem::is_regular_file(status)) {
            const auto relative = entry->path().lexically_relative(root);
            auto name = osal::path_to_utf8(relative);
            if (!name) {
                return std::unexpected(wrap_error(ErrorCode::InvalidPackage, ErrorOperation::Load, name.error()));
            }

            std::replace(name->begin(), name->end(), '\\', '/');
            if (!safe_path(*name)) {
                return std::unexpected(failure(ErrorCode::InvalidPackage));
            }

            auto bytes = read_file(entry->path(), budget);
            if (!bytes) {
                return std::unexpected(bytes.error());
            }

            files.emplace(std::move(*name), std::move(*bytes));
        } else if (!std::filesystem::is_directory(status)) {
            return std::unexpected(failure(ErrorCode::InvalidPackage));
        }

        entry.increment(error);
    }

    if (error) {
        return std::unexpected(file_failure(error));
    }

    return files;
}

std::string digest(const Files& files) {
    picosha2::hash256_one_by_one hasher;
    for (const auto& [name, bytes] : files) {
        // Length framing plus ASCII relative paths makes the package digest
        // independent of directory enumeration order and native path separators.
        const auto header = name + '\0' + std::to_string(bytes.size()) + '\0';
        hasher.process(header.begin(), header.end());
        hasher.process(bytes.begin(), bytes.end());
    }

    hasher.finish();
    return picosha2::get_hash_hex_string(hasher);
}

std::string mime(std::string_view name) {
    const auto extension = name.substr(name.find_last_of('.') + 1);
    if (extension == "html") {
        return "text/html; charset=utf-8";
    }
    if (extension == "js") {
        return "text/javascript; charset=utf-8";
    }
    if (extension == "css") {
        return "text/css; charset=utf-8";
    }
    if (extension == "json") {
        return "application/json";
    }
    if (extension == "txt") {
        return "text/plain; charset=utf-8";
    }
    if (extension == "svg") {
        return "image/svg+xml";
    }
    if (extension == "png") {
        return "image/png";
    }
    if (extension == "woff2") {
        return "font/woff2";
    }

    return "application/octet-stream";
}

Result<Json> manifest(const Files& files) {
    const auto found = files.find("plugin.json");
    if (found == files.end() || found->second.size() > config::ui_manifest_bytes) {
        return std::unexpected(failure(ErrorCode::InvalidPackage));
    }

    auto value = Json::parse(found->second, nullptr, false);
    if (!value.is_object()) {
        return std::unexpected(failure(ErrorCode::InvalidPackage));
    }

    for (const auto* field : {"id", "name", "version", "entry"}) {
        if (!value.contains(field) || !value[field].is_string() ||
            value[field].get_ref<const std::string&>().size() > config::ui_label_bytes || value[field] == "") {
            return std::unexpected(failure(ErrorCode::InvalidPackage));
        }
    }

    const auto id = value["id"].get<std::string>();
    const auto entry = value["entry"].get<std::string>();
    if (!identifier(id) || !safe_path(entry) || !entry.ends_with(".js") || !files.contains(entry) ||
        !value.contains("apiVersion") || !value["apiVersion"].is_number_integer() || value["apiVersion"] != 1 ||
        !value.contains("panels") || !value["panels"].is_array() || value["panels"].empty() ||
        value["panels"].size() > config::ui_panels_per_plugin) {
        return std::unexpected(failure(ErrorCode::InvalidPackage));
    }

    std::set<std::string> panels;
    for (const auto& panel : value["panels"]) {
        if (!panel.is_object() || !panel.contains("id") || !panel["id"].is_string() ||
            !panel.contains("title") || !panel["title"].is_string() ||
            !panel.contains("multiple") || !panel["multiple"].is_boolean()) {
            return std::unexpected(failure(ErrorCode::InvalidPackage));
        }

        const auto panel_id = panel["id"].get<std::string>();
        const auto title = panel["title"].get<std::string>();
        if (!identifier(panel_id) || title.empty() || title.size() > config::ui_label_bytes ||
            !panels.insert(panel_id).second) {
            return std::unexpected(failure(ErrorCode::InvalidPackage));
        }
    }

    // Only these fields are part of the v1 wire contract. A package cannot
    // self-assert official status, a digest, or a URL outside its own snapshot.
    return Json{{"id", id}, {"name", value["name"]}, {"version", value["version"]},
                {"entry", entry}, {"apiVersion", 1}, {"panels", value["panels"]}};
}
}

Result<Assets> load_assets(const std::filesystem::path& supplied_root) {
    std::error_code error;
    const auto root = std::filesystem::absolute(supplied_root, error).lexically_normal();
    if (error) {
        return std::unexpected(file_failure(error));
    }

    std::size_t budget = config::ui_snapshot_bytes;
    auto app = read_package(root / "app", budget);
    if (!app) {
        return std::unexpected(app.error());
    }
    if (!app->contains("index.html")) {
        return std::unexpected(failure(ErrorCode::InvalidPackage));
    }

    Assets assets;
    for (auto& [path, bytes] : *app) {
        assets.routes.emplace('/' + path, Asset{std::move(bytes), mime(path)});
    }

    const auto plugin_root = root / "plugins";
    auto checked = check_directory(plugin_root);
    if (!checked) {
        return std::unexpected(checked.error());
    }

    struct Package {
        Files files;
        Json info;
    };
    std::vector<Package> packages;
    Json diagnostics = Json::array();
    std::map<std::string, std::size_t> counts;
    std::filesystem::directory_iterator entry(plugin_root, error);
    const std::filesystem::directory_iterator end;
    std::size_t entries = 0;
    while (!error && entry != end) {
        if (++entries > config::ui_plugin_limit) {
            return std::unexpected(failure(ErrorCode::CapacityExceeded));
        }

        auto files = read_package(entry->path(), budget);
        if (files) {
            auto info = manifest(*files);
            if (info) {
                ++counts[(*info)["id"].get<std::string>()];
                (*info)["digest"] = digest(*files);
                packages.push_back({std::move(*files), std::move(*info)});
            } else {
                diagnostics.push_back("Plugin package rejected: invalid manifest or unsupported API version.");
            }
        } else {
            diagnostics.push_back("Plugin package rejected: unreadable, unsafe, or over budget.");
        }

        entry.increment(error);
    }

    if (error) {
        return std::unexpected(file_failure(error));
    }

    Json plugins = Json::array();
    std::sort(packages.begin(), packages.end(), [](const auto& left, const auto& right) {
        return left.info["id"] < right.info["id"];
    });
    for (auto& package : packages) {
        const auto id = package.info["id"].get<std::string>();
        if (counts[id] != 1) {
            diagnostics.push_back("Duplicate plugin ID rejected: " + id);
            continue;
        }

        const auto hash = package.info["digest"].get<std::string>();
        const auto prefix = "/plugins/" + id + '/' + hash + '/';
        package.info["entry"] = prefix + package.info["entry"].get<std::string>();
        package.info["official"] = id == "vinc.example" && hash == official_package_hash;
        for (auto& [path, bytes] : package.files) {
            assets.routes.emplace(prefix + path, Asset{std::move(bytes), mime(path)});
        }

        plugins.push_back(std::move(package.info));
    }

    assets.catalog = Json{{"apiVersion", 1}, {"plugins", plugins}, {"diagnostics", diagnostics}}.dump();
    return assets;
}
}

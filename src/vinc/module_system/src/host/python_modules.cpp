#include <vinc/module/host/python_modules.hpp>
#include <vinc/module/detail/python_launch.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/osal/error.hpp>
#include <vinc/param/types.hpp>
#include <algorithm>
#include <cstdlib>

namespace vinc {
namespace {
using module_system::ErrorCode;
using module_system::ErrorOperation;

Error discovery_error(const std::error_code& error) {
    return Error{ErrorCode::PythonDiscoveryFailed, ErrorOperation::DiscoverPython,
                 osal::native_error(error)};
}

Result<std::filesystem::path> system_python() {
    // Read PATH in its native representation: Windows environment paths are UTF-16.
#ifdef _WIN32
#ifdef _MSC_VER
    wchar_t* environment = nullptr;
    std::size_t length = 0;
    const auto copied = _wdupenv_s(&environment, &length, L"PATH");
    const std::unique_ptr<wchar_t, decltype(&std::free)> owner(environment, &std::free);
    if (copied != 0) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Spawn,
            osal::native_error(std::error_code(copied, std::generic_category()))});
    }

#else
    const auto* environment = _wgetenv(L"PATH");
#endif
    const std::wstring paths = environment ? environment : L"";
    constexpr wchar_t separator = L';';
    const std::filesystem::path filename = L"python.exe";
#else
    const auto* environment = std::getenv("PATH");
    const std::string paths = environment ? environment : "";
    constexpr char separator = ':';
    const std::filesystem::path filename = "python3";
#endif
    std::size_t start = 0;
    while (start <= paths.size()) {
        const auto end = paths.find(separator, start);
        const auto part = paths.substr(start, end - start);
        if (!part.empty()) {
            std::error_code error;
            auto candidate = std::filesystem::absolute(std::filesystem::path(part) / filename, error);
            if (!error && std::filesystem::is_regular_file(candidate, error) && !error) {
                return candidate;
            }
        }

        if (end == paths.npos) {
            break;
        }

        start = end + 1;
    }

    return std::unexpected(Error{ErrorCode::PythonUnavailable, ErrorOperation::Spawn});
}
}

Result<void> detail::prepare_python_spawn(osal::SpawnOptions& options, const PythonLaunch& launch) {
    auto executable = launch.executable.empty() ? system_python() : Result<std::filesystem::path>(launch.executable);
    if (!executable) {
        return std::unexpected(executable.error());
    }

    auto bootstrap = osal::path_to_utf8(launch.bootstrap);
    if (!bootstrap) {
        return std::unexpected(wrap_error(ErrorCode::InvalidArgument, ErrorOperation::Spawn, bootstrap.error()));
    }

    options.executable = std::move(*executable);
    // -u keeps Python print/traceback visible through the existing output capture.
    options.arguments.insert(options.arguments.begin(), {"-u", *bootstrap});
    options.working_directory = launch.script.parent_path();
    options.environment.push_back({"PYTHONIOENCODING", "utf-8"});
    return {};
}

Result<PythonDiscoveryReport> discover_python_modules(ModuleManager& manager,
    const std::filesystem::path& root, const std::filesystem::path& bootstrap,
    const std::filesystem::path& executable) {
    if (!root.is_absolute() || !bootstrap.is_absolute() || (!executable.empty() && !executable.is_absolute())) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::DiscoverPython});
    }

    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        if (error) {
            return std::unexpected(discovery_error(error));
        }

        return PythonDiscoveryReport{};
    }

    std::vector<std::filesystem::path> entries;
    std::filesystem::directory_iterator iterator(root, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        entries.push_back(iterator->path());
        iterator.increment(error);
    }

    if (error) {
        return std::unexpected(discovery_error(error));
    }

    std::sort(entries.begin(), entries.end());
    PythonDiscoveryReport report;
    for (const auto& directory : entries) {
        const auto status = std::filesystem::symlink_status(directory, error);
        if (error) {
            report.issues.push_back({directory, discovery_error(error)});
            error.clear();
            continue;
        }

        if (!std::filesystem::is_directory(status)) {
            continue;
        }

        auto name = osal::path_to_utf8(directory.filename());
        if (!name || !param::valid_segment(*name)) {
            report.issues.push_back({directory, Error{ErrorCode::InvalidName, ErrorOperation::DiscoverPython}});
            continue;
        }

        auto script = directory / directory.filename();
        script += ".py";
        if (!std::filesystem::is_regular_file(script, error)) {
            report.issues.push_back({script, discovery_error(error)});
            error.clear();
            continue;
        }

        ModuleType type;
        type.name = *name;
        type.python = PythonLaunch{script, bootstrap, executable};
        auto registered = manager.register_module_type(std::move(type));
        if (!registered) {
            report.issues.push_back({script, registered.error()});
            continue;
        }
        ++report.registered;
    }

    return report;
}
}

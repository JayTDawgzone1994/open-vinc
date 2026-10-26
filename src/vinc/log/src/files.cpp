#include <vinc/log/detail/files.hpp>
#include <vinc/osal/error.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/pattern_formatter.h>
#include <algorithm>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>

namespace vinc::log {
namespace {
std::tm local_time(std::chrono::system_clock::time_point time) {
    auto seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    return local;
}

std::string escape(std::string_view input) {
    std::string output;
    for (unsigned char ch : input) {
        if (ch == '\n') {
            output += "\\n";
        } else if (ch == '\r') {
            output += "\\r";
        } else if (ch == '\t') {
            output += "\\t";
        } else if (ch == '\\') {
            output += "\\\\";
        } else if (ch < 32 || ch == 127) {
            output += fmt::format("\\x{:02x}", ch);
        } else {
            output += static_cast<char>(ch);
        }
    }
    return output;
}
}

std::string date_folder(std::chrono::system_clock::time_point time) {
    const auto local = local_time(time);
    std::ostringstream text;
    text << std::put_time(&local, "%Y-%m-%d");
    return text.str();
}

std::string render(const Record& record) {
    const auto local = local_time(record.received);
    std::ostringstream stamp;
    stamp << std::put_time(&local, "%Y-%m-%dT%H:%M:%S%z");
    const char* level = "RAW";
    if (record.level) {
        constexpr const char* names[]{"ERROR", "WARN", "INFO", "DEBUG"};
        level = names[static_cast<std::size_t>(*record.level)];
    }
    const char* origin = "structured";
    if (record.origin == Origin::Stdout) {
        origin = "stdout";
    } else if (record.origin == Origin::Stderr) {
        origin = "stderr";
    } else if (record.origin == Origin::Diagnostic) {
        origin = "diagnostic";
    }
    return fmt::format(
        "{} seq={} vinc={} run={} module={} mid={} pid={} origin={} level={} produced_us={} received_us={} "
        "mono_us={} {}",
        stamp.str(), record.sequence, escape(record.instance), escape(record.startup), escape(record.module),
        record.mid, record.pid, origin, level,
        record.produced_us ? std::to_string(*record.produced_us) : "unknown",
        std::chrono::duration_cast<std::chrono::microseconds>(record.received.time_since_epoch()).count(),
        std::chrono::duration_cast<std::chrono::microseconds>(record.monotonic.time_since_epoch()).count(),
        escape(record.text));
}
}

namespace vinc::log::detail {
namespace {
Error file_error(ErrorOperation operation, std::error_code error = {}) {
    return {ErrorCode::FileFailed, operation, osal::native_error(error)};
}

bool valid_day(std::string_view day) {
    if (day.size() != 10 || day[4] != '-' || day[7] != '-') {
        return false;
    }
    for (std::size_t i = 0; i < day.size(); ++i) {
        if (i != 4 && i != 7 && (day[i] < '0' || day[i] > '9')) {
            return false;
        }
    }
    return true;
}

std::optional<std::uint64_t> managed_sequence(std::string_view name) {
    constexpr std::size_t digits = 20;
    if (name.size() != 5 + digits + 4 || !name.starts_with("vinc-") || !name.ends_with(".log")) {
        return {};
    }
    std::uint64_t value = 0;
    auto parsed = std::from_chars(name.data() + 5, name.data() + 5 + digits, value);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + 5 + digits || value == 0) {
        return {};
    }
    return value;
}
}

struct Files::Impl {
    struct File {
        std::filesystem::path path;
        std::uint64_t bytes;
    };

    std::filesystem::path root;
    FileOptions options;
    std::map<std::uint64_t, File> files;
    std::uint64_t total = 0;
    std::uint64_t next = 1;
    std::uint64_t current = 0;
    std::string day;
    std::unique_ptr<spdlog::sinks::basic_file_sink_mt> sink;

    Result<void> scan() {
        std::error_code error;
        std::filesystem::directory_iterator folders(root, error);
        if (error) {
            return std::unexpected(file_error(ErrorOperation::Configure, error));
        }
        const std::filesystem::directory_iterator end;
        while (folders != end) {
            const auto folder = *folders;
            auto status = folder.symlink_status(error);
            if (error) {
                return std::unexpected(file_error(ErrorOperation::Configure, error));
            }
            if (std::filesystem::is_directory(status) && valid_day(folder.path().filename().string())) {
                std::filesystem::directory_iterator entries(folder.path(), error);
                if (error) {
                    return std::unexpected(file_error(ErrorOperation::Configure, error));
                }
                while (entries != end) {
                    const auto entry = *entries;
                    auto kind = entry.symlink_status(error);
                    if (error) {
                        return std::unexpected(file_error(ErrorOperation::Configure, error));
                    }
                    const auto sequence = managed_sequence(entry.path().filename().string());
                    if (sequence && std::filesystem::is_regular_file(kind)) {
                        const auto size = entry.file_size(error);
                        if (error || size > UINT64_MAX - total || files.contains(*sequence) ||
                            *sequence == UINT64_MAX) {
                            return std::unexpected(file_error(ErrorOperation::Configure, error));
                        }
                        files.emplace(*sequence, File{entry.path(), size});
                        total += size;
                        next = (std::max)(next, *sequence + 1);
                    }
                    entries.increment(error);
                    if (error) {
                        return std::unexpected(file_error(ErrorOperation::Configure, error));
                    }
                }
            }
            folders.increment(error);
            if (error) {
                return std::unexpected(file_error(ErrorOperation::Configure, error));
            }
        }
        return trim(0);
    }

    Result<void> trim(std::uint64_t incoming) {
        while (total > options.max_total_bytes - incoming) {
            if (files.empty() || files.begin()->first == current) {
                return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Rotate});
            }
            const auto item = files.begin();
            std::error_code error;
            const auto status = std::filesystem::symlink_status(item->second.path, error);
            if (error || !std::filesystem::is_regular_file(status)) {
                return std::unexpected(file_error(ErrorOperation::Rotate, error));
            }
            if (!std::filesystem::remove(item->second.path, error) || error) {
                return std::unexpected(file_error(ErrorOperation::Rotate, error));
            }
            total -= item->second.bytes;
            const auto parent = item->second.path.parent_path();
            files.erase(item);
            // Only remove an empty managed date directory; never recurse.
            if (std::filesystem::is_empty(parent, error) && !error) {
                std::filesystem::remove(parent, error);
            }
        }
        return {};
    }

    Result<void> rotate(std::string_view date) {
        if (sink) {
            sink->flush();
        }
        sink.reset();
        current = 0;
        const auto folder = root / std::string(date);
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (error || !std::filesystem::is_directory(std::filesystem::symlink_status(folder, error))) {
            return std::unexpected(file_error(ErrorOperation::Rotate, error));
        }
        if (next == UINT64_MAX) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Rotate});
        }
        auto path = folder / fmt::format("vinc-{:020}.log", next);
        const auto destination = std::filesystem::symlink_status(path, error);
        if ((error && error != std::errc::no_such_file_or_directory) ||
            destination.type() != std::filesystem::file_type::not_found) {
            return std::unexpected(file_error(ErrorOperation::Rotate, error));
        }
        // Directory is framework-owned. No concurrent external writers are supported.
        sink = std::make_unique<spdlog::sinks::basic_file_sink_mt>(path.native(), false);
        sink->set_formatter(
            std::make_unique<spdlog::pattern_formatter>("%v", spdlog::pattern_time_type::local, "\n"));
        current = next++;
        files.emplace(current, File{std::move(path), 0});
        day = date;
        return {};
    }
};

Files::Files() : impl_(std::make_unique<Impl>()) {
}

Files::~Files() = default;

Result<void> Files::configure(std::filesystem::path root, FileOptions options) {
    if (!options.max_file_bytes || options.max_total_bytes < options.max_file_bytes || root.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Configure});
    }
    auto flushed = flush();
    impl_->sink.reset();
    impl_->current = 0;
    if (!flushed) {
        return flushed;
    }
    auto replacement = std::make_unique<Impl>();
    replacement->root = std::move(root);
    replacement->options = options;
    std::error_code error;
    // Reject symlink ancestors rather than letting retention leave its namespace.
    for (auto path = replacement->root; !path.empty(); path = path.parent_path()) {
        const auto status = std::filesystem::symlink_status(path, error);
        if (!error && std::filesystem::is_symlink(status)) {
            return std::unexpected(file_error(ErrorOperation::Configure));
        }
        if (path == path.parent_path()) {
            break;
        }
        error.clear();
    }
    std::filesystem::create_directories(replacement->root, error);
    if (error) {
        return std::unexpected(file_error(ErrorOperation::Configure, error));
    }
    auto scanned = replacement->scan();
    if (!scanned) {
        return scanned;
    }
    impl_ = std::move(replacement);
    return {};
}

Result<void> Files::append(std::string_view day, std::string_view encoded) {
    if (!valid_day(day)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Write});
    }
    const auto bytes = encoded.size() + 1; // Sink uses exactly one LF, including Windows.
    if (bytes > impl_->options.max_file_bytes) {
        return std::unexpected(Error{ErrorCode::TooLarge, ErrorOperation::Write});
    }
    try {
        const bool full =
            impl_->current && bytes > impl_->options.max_file_bytes - impl_->files.at(impl_->current).bytes;
        if (!impl_->sink || day != impl_->day || full ||
            impl_->total > impl_->options.max_total_bytes - bytes) {
            auto rotated = impl_->rotate(day);
            if (!rotated) {
                return rotated;
            }
        }
        auto trimmed = impl_->trim(bytes);
        if (!trimmed) {
            return trimmed;
        }
        spdlog::details::log_msg message({}, spdlog::level::info,
                                         spdlog::string_view_t(encoded.data(), encoded.size()));
        impl_->sink->log(message);
        impl_->total += bytes;
        impl_->files.at(impl_->current).bytes += bytes;
        return {};
    } catch (const spdlog::spdlog_ex&) {
        // Third-party I/O exceptions end here. Never call spdlog's default stderr handler.
        return std::unexpected(
            file_error(ErrorOperation::Write, std::error_code(errno, std::generic_category())));
    }
}

Result<void> Files::flush() {
    try {
        if (impl_->sink) {
            impl_->sink->flush();
        }
        return {};
    } catch (const spdlog::spdlog_ex&) {
        return std::unexpected(
            file_error(ErrorOperation::Flush, std::error_code(errno, std::generic_category())));
    }
}

std::uint64_t Files::bytes() const {
    return impl_->total;
}
}

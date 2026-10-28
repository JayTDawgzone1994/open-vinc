#include <vinc/module/detail/instance.hpp>
#include <vinc/module/error.hpp>
#include <vinc/config.hpp>
#include <vinc/osal/random.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/notification.hpp>
#include <array>
#include <fstream>
#include <vector>
#include <cstdlib>

namespace vinc::detail {
namespace {
using module_system::ErrorCode;
using module_system::ErrorOperation;
namespace fs = std::filesystem;
// Journal v1 encoding limits, also validated by the CMake VINC_NAME option.
constexpr std::size_t maximum_instance_name_bytes = 48;
constexpr std::size_t epoch_digits = 12;
constexpr std::size_t sequence_digits = 11;
constexpr std::size_t resource_name_bytes = 1 + epoch_digits + sequence_digits;
constexpr char hex[] = "0123456789abcdef";

Error storage_error(const std::error_code& native, ErrorOperation operation) {
    return Error{ErrorCode::InstanceStorageFailed, operation, osal::native_error(native)};
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > maximum_instance_name_bytes) {
        return false;
    }
    for (const auto ch : name) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

bool hex_string(std::string_view text) {
    for (const auto ch : text) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

Result<void> private_directory(const fs::path& path) {
    std::error_code error;
    const bool created = fs::create_directory(path, error);
    if (error) {
        return std::unexpected(storage_error(error, ErrorOperation::OpenInstance));
    }
    const auto status = fs::symlink_status(path, error);
    if (error) {
        return std::unexpected(storage_error(error, ErrorOperation::OpenInstance));
    }
    if (status.type() != fs::file_type::directory) {
        return std::unexpected(Error{ErrorCode::InvalidInstanceJournal, ErrorOperation::OpenInstance});
    }
#ifndef _WIN32
    if (created) {
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, error);
        if (error) {
            return std::unexpected(storage_error(error, ErrorOperation::OpenInstance));
        }
    } else if ((status.permissions() & (fs::perms::group_all | fs::perms::others_all)) != fs::perms::none) {
        return std::unexpected(Error{ErrorCode::InvalidInstanceJournal, ErrorOperation::OpenInstance});
    }
#else
    (void)created;
#endif
    return {};
}

Result<void> remove_record(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error); // Deliberately nonrecursive: unexpected contents fail closed.
    if (error) {
        return std::unexpected(storage_error(error, ErrorOperation::CleanInstance));
    }
    return {};
}

Result<void> clean_epoch(const fs::path& path, const std::string& epoch) {
    std::error_code error;
    fs::directory_iterator entries(path, error);
    if (error) {
        return std::unexpected(storage_error(error, ErrorOperation::CleanInstance));
    }
    // Validate the entire small journal before deleting any named OS resource.
    std::vector<fs::path> records;
    for (const auto& entry : entries) {
        const auto filename = entry.path().filename().string();
        if (filename == "owner") {
            continue;
        }
        if (records.size() >= config::instance_resource_limit || filename.size() != resource_name_bytes ||
            (filename.front() != 'r' && filename.front() != 'n') ||
            filename.substr(1, epoch_digits) != epoch ||
            !hex_string(std::string_view(filename).substr(1 + epoch_digits)) ||
            entry.symlink_status().type() != fs::file_type::directory || !fs::is_empty(entry.path())) {
            return std::unexpected(Error{ErrorCode::InvalidInstanceJournal, ErrorOperation::CleanInstance});
        }
        records.push_back(entry.path());
    }
    for (const auto& record : records) {
#ifndef _WIN32
        const auto name = record.filename().string();
        auto unlinked = name.front() == 'n' ? osal::NotificationReceiver::unlink_name(name)
                                           : osal::SharedMemory::unlink_name(name);
        if (!unlinked && !unlinked.error().is(osal::ErrorCode::NotFound)) {
            return std::unexpected(wrap_error(ErrorCode::CleanupFailed, ErrorOperation::CleanInstance,
                                               unlinked.error()));
        }
#endif
        // Windows names follow handle lifetime; never reopen or mutate old mappings.
        auto removed = remove_record(record);
        if (!removed) {
            return removed;
        }
    }
    auto removed_owner = remove_record(path / "owner");
    if (!removed_owner) {
        return removed_owner;
    }
    // Keep the empty prefix reservation: an old Windows view may still exist.
    // A later generation must never recreate that native name, even by chance.
    return {};
}
}

Result<void> Instance::prepare_directory() {
    std::error_code error;
#ifdef _WIN32
    root_ = fs::temp_directory_path(error) / "open-vinc-state-v1";
#else
    // HOME is an OS-native pathname, not an encoded wire string. A per-user
    // directory avoids sharing /tmp ownership between unrelated user accounts.
    const char* home_directory = std::getenv("HOME");
    if (!home_directory || !fs::path(home_directory).is_absolute()) {
        return std::unexpected(Error{ErrorCode::InstanceStorageFailed, ErrorOperation::OpenInstance});
    }
    root_ = fs::path(home_directory) / ".open-vinc-state-v1";
#endif
    if (error) {
        return std::unexpected(storage_error(error, ErrorOperation::OpenInstance));
    }
    auto root = private_directory(root_);
    if (!root) {
        return root;
    }
    return private_directory(root_ / "epochs");
}

Result<void> Instance::acquire_instance() {
    const auto path = root_ / ("instance-" + name_ + ".lock");
    auto file = osal::FileLock::create(path);
    if (!file && file.error().is(osal::ErrorCode::AlreadyExists)) {
        file = osal::FileLock::open(path);
    }
    if (!file) {
        return std::unexpected(wrap_error(ErrorCode::InstanceStorageFailed, ErrorOperation::OpenInstance,
                                           file.error()));
    }
    file_ = std::move(*file);
    auto locked = file_.try_lock();
    if (!locked) {
        const auto code = locked.error().is(osal::ErrorCode::WouldBlock)
                            ? ErrorCode::InstanceRunning : ErrorCode::InstanceStorageFailed;
        return std::unexpected(wrap_error(code, ErrorOperation::OpenInstance, locked.error()));
    }
    guard_.emplace(std::move(*locked));
    return {};
}

Result<void> Instance::clean_previous() {
    std::error_code error;
    fs::directory_iterator epochs(root_ / "epochs", error);
    if (error) {
        return std::unexpected(storage_error(error, ErrorOperation::CleanInstance));
    }
    std::size_t epoch_count = 0;
    for (const auto& entry : epochs) {
        if (++epoch_count >= config::instance_epoch_limit) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::CleanInstance});
        }
        const auto epoch = entry.path().filename().string();
        if (epoch.size() != epoch_digits || !hex_string(epoch) ||
            entry.symlink_status().type() != fs::file_type::directory) {
            continue;
        }
        const auto owner_path = entry.path() / "owner";
        const auto owner_status = fs::symlink_status(owner_path, error);
        if (owner_status.type() == fs::file_type::not_found) {
            error.clear();
            continue; // Interrupted admission cannot have created shared resources yet.
        }
        if (error) {
            return std::unexpected(storage_error(error, ErrorOperation::CleanInstance));
        }
        if (owner_status.type() != fs::file_type::regular) {
            return std::unexpected(Error{ErrorCode::InvalidInstanceJournal, ErrorOperation::CleanInstance});
        }
        std::ifstream owner(owner_path, std::ios::binary);
        if (!owner) {
            return std::unexpected(Error{ErrorCode::InstanceStorageFailed, ErrorOperation::CleanInstance});
        }
        std::array<char, maximum_instance_name_bytes + 2> text{};
        owner.read(text.data(), text.size());
        if (owner.bad()) {
            return std::unexpected(Error{ErrorCode::InstanceStorageFailed, ErrorOperation::CleanInstance});
        }
        const auto bytes = owner.gcount();
        const std::string_view recorded(text.data(), static_cast<std::size_t>(bytes));
        if (recorded != name_ + "\n") {
            continue;
        }
        owner.close();
        auto cleaned = clean_epoch(entry.path(), epoch);
        if (!cleaned) {
            return cleaned;
        }
    }
    return {};
}

Result<void> Instance::create_epoch() {
    // Globally reserve the prefix in this state directory, including across Vinc
    // names. No cooperating instance can allocate the same live resource names.
    for (std::size_t attempt = 0; attempt < config::instance_name_attempts; ++attempt) {
        std::array<std::byte, epoch_digits / 2> random;
        auto generated = osal::random_bytes(random);
        if (!generated) {
            return std::unexpected(wrap_error(ErrorCode::InstanceStorageFailed, ErrorOperation::OpenInstance,
                                               generated.error()));
        }
        std::string candidate;
        for (const auto byte : random) {
            const auto value = std::to_integer<unsigned>(byte);
            candidate += hex[value >> 4];
            candidate += hex[value & 15];
        }
        const auto path = root_ / "epochs" / candidate;
        std::error_code error;
        if (!fs::create_directory(path, error)) {
            if (error) {
                return std::unexpected(storage_error(error, ErrorOperation::OpenInstance));
            }
            continue;
        }
        std::ofstream owner(path / "owner", std::ios::binary);
        owner << name_ << '\n';
        owner.close();
        if (!owner) {
            return std::unexpected(Error{ErrorCode::InstanceStorageFailed, ErrorOperation::OpenInstance});
        }
        epoch_ = std::move(candidate);
        epoch_path_ = path;
        return {};
    }
    return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::OpenInstance});
}

Result<std::shared_ptr<Instance>> Instance::create(std::string name) {
    if (!valid_name(name)) {
        return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::OpenInstance});
    }
    auto instance = std::shared_ptr<Instance>(new Instance());
    instance->name_ = std::move(name);
    try {
        auto directory = instance->prepare_directory();
        if (!directory) {
            return std::unexpected(directory.error());
        }
        auto acquired = instance->acquire_instance();
        if (!acquired) {
            return std::unexpected(acquired.error());
        }
        auto cleaned = instance->clean_previous();
        if (!cleaned) {
            return std::unexpected(cleaned.error());
        }
        auto epoch = instance->create_epoch();
        if (!epoch) {
            return std::unexpected(epoch.error());
        }
        return instance;
    } catch (const fs::filesystem_error& error) {
        // Iterator/status operations may fail after initial construction too.
        return std::unexpected(storage_error(error.code(), ErrorOperation::OpenInstance));
    }
}

Instance::~Instance() = default; // Retain unretired records for the next admitted central.

Result<std::string> Instance::reserve() {
    return reserve_kind('r');
}

Result<std::string> Instance::reserve_notification() {
    return reserve_kind('n');
}

Result<std::string> Instance::reserve_kind(char kind) {
    std::lock_guard lock(mutex_);
    constexpr auto maximum_sequence = (std::uint64_t{1} << (sequence_digits * 4)) - 1;
    if (recorded_ >= config::instance_resource_limit || next_ > maximum_sequence) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::RecordResource});
    }
    std::string name(1, kind);
    name += epoch_;
    auto sequence = std::uint64_t(next_++);
    std::array<char, sequence_digits> suffix{};
    for (std::size_t digit = suffix.size(); digit > 0; --digit) {
        suffix[digit - 1] = hex[sequence & 15];
        sequence >>= 4;
    }
    name.append(suffix.data(), suffix.size());
    std::error_code error;
    if (!fs::create_directory(epoch_path_ / name, error)) {
        return std::unexpected(storage_error(error, ErrorOperation::RecordResource));
    }
    ++recorded_;
    return name;
}

Result<void> Instance::retire(std::string_view name) {
    if (name.size() != resource_name_bytes || (name.front() != 'r' && name.front() != 'n') ||
        name.substr(1, epoch_digits) != epoch_ ||
        !hex_string(name.substr(1 + epoch_digits))) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::CleanInstance});
    }
    std::lock_guard lock(mutex_);
    std::error_code error;
    const bool removed = fs::remove(epoch_path_ / name, error);
    if (error) {
        return std::unexpected(storage_error(error, ErrorOperation::CleanInstance));
    }
    if (removed) {
        --recorded_;
    }
    return {};
}

const std::string& Instance::name() const noexcept {
    return name_;
}

const std::filesystem::path& Instance::journal() const noexcept {
    return epoch_path_;
}
}

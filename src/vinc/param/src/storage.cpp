#include <vinc/param/detail/storage.hpp>
#include <vinc/owner_id.hpp>
#include <cstring>

namespace vinc::param::detail {
namespace {
Error failed(const Error& error, ErrorOperation operation) {
    return wrap_error(ErrorCode::StorageFailed, operation, error);
}
}

Result<std::unique_ptr<Storage>> Storage::create(std::shared_ptr<vinc::detail::ResourceNames> names,
                                                 std::string_view lock_name, std::uint64_t lock_id) {
    auto result = std::make_unique<Storage>();
    result->names_ = std::move(names);
    auto name = result->names_->reserve();
    if (!name) {
        return std::unexpected(failed(name.error(), ErrorOperation::Create));
    }

    result->descriptor_ = {lock_id, std::move(*name)};
    auto opened = result->open_mutex(lock_name);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    auto image = result->create_image();
    if (!image) {
        return std::unexpected(image.error());
    }

    auto initialized = result->publish(Snapshot{});
    if (!initialized) {
        return std::unexpected(initialized.error());
    }

    return result;
}

Result<void> Storage::open_mutex(std::string_view name) {
    auto mutex = concurrency::InterprocessMutex::open(name, central_owner);
    if (!mutex) {
        return std::unexpected(failed(mutex.error(), ErrorOperation::Create));
    }
    mutex_ = std::move(*mutex);
    return {};
}

Result<void> Storage::create_image() {
    auto memory = osal::SharedMemory::create(descriptor_.data_name, storage_bytes);
    if (!memory) {
        return std::unexpected(failed(memory.error(), ErrorOperation::Create));
    }
    memory_ = std::move(*memory);
    created_ = true;
    auto mapping = memory_.map(storage_bytes, osal::SharedMemoryAccess::ReadWrite);
    if (!mapping) {
        return std::unexpected(failed(mapping.error(), ErrorOperation::Create));
    }
    mapping_ = std::move(*mapping);
    return {};
}

Storage::~Storage() {
    (void)mapping_.unmap();
    (void)memory_.close();
    if (created_) {
        try {
            auto removed = osal::SharedMemory::unlink_name(descriptor_.data_name);
            if (removed || removed.error().is(osal::ErrorCode::NotFound) ||
                removed.error().is(osal::ErrorCode::Unsupported)) {
                (void)names_->retire(descriptor_.data_name);
            }
        } catch (...) {
            // The instance journal retains cleanup work if filesystem allocation fails.
        }
    }
}

Result<void> Storage::publish(const Snapshot& snapshot, std::stop_token stop) {
    auto encoded = encode_snapshot(snapshot);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    control::Bytes header;
    control::put_u64(header, snapshot.revision);
    control::put_u64(header, encoded->size());
    auto writable = mapping_.writable_bytes();
    if (!writable) {
        return std::unexpected(failed(writable.error(), ErrorOperation::Set));
    }

    auto guard = mutex_.lock_until(std::chrono::steady_clock::now() + config::param_lock_timeout, stop);
    if (!guard) {
        return std::unexpected(failed(guard.error(), ErrorOperation::Set));
    }

    const auto destination = writable->subspan(guard->publication() ? 0 : slot_extent, slot_extent);
    // Wire bytes have no native C++ object lifetime/alignment requirements. A failed
    // writer leaves this candidate invisible until the single publication CAS.
    std::memcpy(destination.data(), header.data(), header.size());
    std::memcpy(destination.data() + slot_header_bytes, encoded->data(), encoded->size());
    auto committed = guard->publish_and_unlock();
    if (!committed) {
        return std::unexpected(failed(committed.error(), ErrorOperation::Set));
    }

    return {};
}

Result<std::uint64_t> copy_image(std::span<const std::byte> mapping, bool publication,
                                 std::uint64_t known_revision, control::Bytes& buffer) {
    if (mapping.size() != storage_bytes || buffer.capacity() < config::param_slot_bytes) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Pump});
    }

    auto source = mapping.subspan(publication ? slot_extent : 0, slot_extent);
    control::Reader header(source.first(slot_header_bytes));
    const auto revision = header.u64();
    const auto size = header.u64();
    if (!header.done() || size > config::param_slot_bytes || size == 0) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Pump});
    }

    buffer.clear();
    if (revision != known_revision) {
        buffer.resize(static_cast<std::size_t>(size));
        std::memcpy(buffer.data(), source.data() + slot_header_bytes, buffer.size());
    }
    return revision;
}
}

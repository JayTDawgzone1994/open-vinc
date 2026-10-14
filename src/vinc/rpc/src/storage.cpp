#include <vinc/rpc/detail/storage.hpp>
#include <cstring>
#include <memory>

namespace vinc::rpc::detail {
namespace {
Error storage_error(const Error& error) {
    return wrap_error(ErrorCode::StorageFailed, ErrorOperation::Create, error);
}

std::uint64_t aligned(std::uint64_t size) {
    return (size + 7) & ~std::uint64_t{7};
}

// SharedWord is the OSAL native IPC ABI: 8-byte aligned, 8-byte storage, initialized
// by the creator before publication. Payloads remain encoded bytes, not C++ objects.
osal::SharedWord& publication(std::span<std::byte> bytes) {
    return *reinterpret_cast<osal::SharedWord*>(bytes.data());
}
}

Result<std::unique_ptr<Pool>> Pool::create(std::shared_ptr<vinc::detail::ResourceNames> names,
                                           ServiceOptions options) {
    auto extent = pool_bytes(options);
    if (!extent) {
        return std::unexpected(extent.error());
    }

    auto pool = std::make_unique<Pool>();
    pool->names_ = std::move(names);
    pool->extent_ = *extent;
    auto name = pool->names_->reserve();
    if (!name) {
        return std::unexpected(storage_error(name.error()));
    }

    pool->name_ = std::move(*name);
    auto allocated = pool->allocate();
    if (!allocated) {
        return std::unexpected(allocated.error());
    }

    auto initialized = pool->initialize(options);
    if (!initialized) {
        return std::unexpected(initialized.error());
    }

    return pool;
}

Result<void> Pool::allocate() {
    auto memory = osal::SharedMemory::create(name_, extent_);
    if (!memory) {
        return std::unexpected(storage_error(memory.error()));
    }

    memory_ = std::move(*memory);
    created_ = true;
    auto mapping = memory_.map(extent_, osal::SharedMemoryAccess::ReadWrite);
    if (!mapping) {
        return std::unexpected(storage_error(mapping.error()));
    }

    mapping_ = std::move(*mapping);
    return {};
}

Result<void> Pool::initialize(ServiceOptions options) {
    auto bytes = mapping_.writable_bytes();
    if (!bytes) {
        return std::unexpected(storage_error(bytes.error()));
    }

    auto stride = slot_bytes(options);
    if (!stride) {
        return std::unexpected(stride.error());
    }

    for (std::uint32_t index = 0; index < options.slots; ++index) {
        const auto offset = index * *stride;
        auto request = bytes->subspan(offset);
        auto response = bytes->subspan(offset + 8 + aligned(options.request_bytes));
        std::construct_at(&publication(request));
        std::construct_at(&publication(response));
    }

    return {};
}

Pool::~Pool() {
    (void)mapping_.unmap();
    (void)memory_.close();
    if (created_) {
        try {
            auto removed = osal::SharedMemory::unlink_name(name_);
            if (removed || removed.error().is(osal::ErrorCode::NotFound) ||
                removed.error().is(osal::ErrorCode::Unsupported)) {
                (void)names_->retire(name_);
            }
        } catch (...) {
            // The durable instance journal retains failed cleanup for next startup.
        }
    }
}

Result<TransferMemory> TransferMemory::open(const Transfer& transfer) {
    auto checked = decode_transfer(encode_transfer(transfer));
    if (!checked) {
        return std::unexpected(checked.error());
    }

    auto memory = osal::SharedMemory::open(transfer.memory, osal::SharedMemoryAccess::ReadWrite);
    if (!memory) {
        return std::unexpected(storage_error(memory.error()));
    }

    auto mapping = memory->map(transfer.extent, osal::SharedMemoryAccess::ReadWrite);
    if (!mapping) {
        return std::unexpected(storage_error(mapping.error()));
    }

    TransferMemory value;
    value.transfer_ = transfer;
    value.mapping_ = std::move(*mapping);
    return value;
}

Result<std::span<std::byte>> TransferMemory::region(bool response) {
    auto bytes = mapping_.writable_bytes();
    if (!bytes) {
        return std::unexpected(storage_error(bytes.error()));
    }

    auto offset = transfer_.offset;
    auto size = transfer_.request_bytes;
    if (response) {
        offset += 8 + aligned(transfer_.request_bytes);
        size = transfer_.response_bytes;
    }

    return bytes->subspan(offset, 8 + size);
}

Result<void> TransferMemory::write(bool response, std::span<const std::byte> bytes) {
    auto target = region(response);
    if (!target) {
        return std::unexpected(target.error());
    }

    if (bytes.size() > target->size() - 8) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Send});
    }

    std::memcpy(target->data() + 8, bytes.data(), bytes.size());
    osal::shared_store(publication(*target), transfer_.token);
    return {};
}

Result<control::Bytes> TransferMemory::read(bool response, std::uint64_t size) {
    auto source = region(response);
    if (!source) {
        return std::unexpected(source.error());
    }

    if (size > source->size() - 8 || osal::shared_load(publication(*source)) != transfer_.token) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::Receive});
    }

    // The admitted lease prevents a writer from reusing these bytes until Release.
    return control::Bytes(source->begin() + 8, source->begin() + 8 + size);
}
}

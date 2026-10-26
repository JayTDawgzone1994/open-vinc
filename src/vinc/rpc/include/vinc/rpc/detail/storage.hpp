#pragma once
#include <vinc/rpc/detail/protocol.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/shared_atomic.hpp>
#include <vinc/detail/resource_names.hpp>

namespace vinc::rpc::detail {
class Pool {
public:
    static Result<std::unique_ptr<Pool>> create(std::shared_ptr<vinc::detail::ResourceNames> names,
                                                ServiceOptions options);
    ~Pool();

    const std::string& name() const {
        return name_;
    }

    std::uint64_t extent() const {
        return extent_;
    }

private:
    std::shared_ptr<vinc::detail::ResourceNames> names_;
    std::string name_;
    std::uint64_t extent_ = 0;
    osal::SharedMemory memory_;
    osal::SharedMemoryMapping mapping_;
    bool created_ = false;
    Result<void> allocate();
    Result<void> initialize(ServiceOptions options);
};

// A local mapping retains the pool while one admitted transfer is being copied.
// The central slot lease, not the mapping destructor, authorizes slot reuse.
class TransferMemory {
public:
    static Result<TransferMemory> open(const Transfer& transfer);
    Result<void> write(bool response, std::span<const std::byte> bytes);
    Result<control::Bytes> read(bool response, std::uint64_t size);

private:
    Transfer transfer_;
    osal::SharedMemoryMapping mapping_;
    Result<std::span<std::byte>> region(bool response);
};
}

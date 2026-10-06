#pragma once

// Framework integration contract: central supervision reports only confirmed exits.
// RingBuffer layout and reclamation stay owned by Message Bus; callers never modify its storage.
#include <vinc/time.hpp>
#include <vinc/message_bus/detail/runtime.hpp>

namespace vinc::message_bus::detail {
class BusManager {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit BusManager(std::unique_ptr<Impl>) noexcept;

public:
    static vinc::Result<std::unique_ptr<BusManager>> create(Limits = {});
    ~BusManager();
    // Enqueue only; allocation/mapping runs on the BusManager worker, never supervisor.
    vinc::Result<void> submit(std::uint64_t owner, std::string name, std::shared_ptr<control::Session>,
                              control::Frame);
    void exited(std::uint64_t owner);               // ONLY after Process::try_wait confirms exit
    vinc::Result<void> drain_until(vinc::Deadline); // call after all producer processes have exited
};
} // namespace vinc::message_bus::detail

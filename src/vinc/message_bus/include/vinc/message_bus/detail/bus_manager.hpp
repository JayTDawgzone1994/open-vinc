#pragma once

// Framework integration contract: central supervision reports only confirmed exits.
// RingBuffer layout and reclamation stay owned by Message Bus; callers never modify its storage.
#include <vinc/time.hpp>
#include <vinc/detail/resource_names.hpp>
#include <vinc/message_bus/detail/runtime.hpp>
#include <functional>

namespace vinc::message_bus::detail {
struct PublisherDefaults {
    std::uint32_t slots = config::message_bus_default_slots;
    std::uint64_t max_message_bytes = config::message_bus_default_message_bytes;
};

// Validate against an empty Bus budget, including ring metadata and its topic
// counter. Current allocations and native allocation failures are not bad settings.
Result<void> validate_publisher_defaults(PublisherDefaults, const Limits&);

// Called only by the Bus worker for omitted options. The owner must keep the
// source alive until BusManager destruction has drained and joined that worker.
using PublisherDefaultsSource = std::function<Result<PublisherDefaults>()>;

class BusManager {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit BusManager(std::unique_ptr<Impl>) noexcept;

public:
    static vinc::Result<std::unique_ptr<BusManager>> create(Limits = {},
        std::shared_ptr<vinc::detail::ResourceNames> resources = {}, PublisherDefaultsSource defaults = {});
    ~BusManager();
    // Enqueue only; allocation/mapping runs on the BusManager worker, never supervisor.
    vinc::Result<void> submit(std::uint64_t owner, std::string name, std::shared_ptr<control::Session>,
                              control::Frame, std::string notification_name = {});
    void exited(std::uint64_t owner);               // ONLY after Process::try_wait confirms exit
    vinc::Result<void> drain_until(vinc::Deadline); // call after all producer processes have exited
};
} // namespace vinc::message_bus::detail

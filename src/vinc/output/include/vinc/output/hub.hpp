#pragma once

// Raw process output fanout. No Module, Shell, logging protocol or terminal I/O.
#include <vinc/osal/process_pipe.hpp>
#include <span>

namespace vinc::output {
enum class Stream { Stdout, Stderr };
enum class EventKind { Data, End, Truncated, Failed, Renamed };
struct Source {
    std::uint64_t id = 0, generation = 0;
    std::string name;
    Stream stream = Stream::Stdout;
};
struct Event {
    std::shared_ptr<const Source> source;
    std::uint64_t sequence = 0;
    osal::Deadline received;
    EventKind kind = EventKind::Data;
    std::string bytes;
    std::optional<osal::OsError> error;
};
struct Loss { std::uint64_t bytes = 0, events = 0; };
struct Batch {
    std::vector<std::shared_ptr<const Event>> events;
    Loss skipped;
};
struct QueueLimits { std::size_t bytes = 256 * 1024, events = 128; };
struct Limits {
    std::size_t max_captures = 128, max_subscribers = 8;
    std::size_t total_queue_bytes = 2 * 1024 * 1024;
    std::size_t total_queue_events = 1024;
    std::chrono::milliseconds drain_timeout{2000};
};
struct Stats {
    std::uint64_t received_bytes = 0, truncated_streams = 0, failed_streams = 0;
    std::size_t active_captures = 0;
};
namespace detail { struct Queue; }
// Poll from the destination's own thread. Hub never calls destination code.
// take() transfers shared immutable events; retained batches are caller memory.
class Subscription {
public:
    Subscription() = default;
    Subscription(Subscription&&) noexcept = default;
    Subscription& operator=(Subscription&&) noexcept = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Batch take(std::size_t max_events = 32);
    void set_enabled(bool); // disabling clears this queue; capture keeps draining
    void close() noexcept { queue_.reset(); }
private:
    friend class OutputHub;
    explicit Subscription(std::shared_ptr<detail::Queue> queue) : queue_(std::move(queue)) {}
    std::shared_ptr<detail::Queue> queue_;
};
struct CapturedProcess { osal::Process process; std::uint64_t capture = 0; };
class OutputHub {
public:
    explicit OutputHub(Limits = {});
    ~OutputHub(); // closes readers and joins collection; never kills processes
    OutputHub(const OutputHub&) = delete;
    OutputHub& operator=(const OutputHub&) = delete;
    osal::Result<Subscription> subscribe(QueueLimits = {});
    // Reserves two source identities and buffers before spawn, then captures
    // both outputs regardless of options' output modes. stdin is unchanged.
    osal::Result<CapturedProcess> spawn(osal::ProcessService&, const osal::SpawnOptions&,
        std::string name, std::uint64_t generation);
    void exited(std::uint64_t capture); // start bounded tail drain after observed OS exit
    // Change labels for future events only; already delivered Source objects stay immutable.
    void set_name(std::uint64_t capture, const std::string& name);
    osal::Result<void> wait_until(osal::Deadline); // current captures only; does not close admission
    Stats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

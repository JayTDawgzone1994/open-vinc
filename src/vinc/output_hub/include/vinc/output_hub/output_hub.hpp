#pragma once

// Raw process output fanout. No Module, Shell, logging protocol or terminal I/O.
#include <vinc/time.hpp>
#include <vinc/output_hub/error.hpp>
#include <vinc/osal/process_pipe.hpp>
#include <vinc/config.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vinc::output_hub {
enum class Stream {
    Stdout,
    Stderr
};
enum class EventKind {
    Data,
    End,
    Truncated,
    Failed,
    Renamed
};

struct Source {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::string name;
    Stream stream = Stream::Stdout;
    std::uint64_t pid = 0;
};

struct Event {
    std::shared_ptr<const Source> source;
    std::uint64_t sequence = 0;
    vinc::Deadline received;
    EventKind kind = EventKind::Data;
    std::string bytes;
    std::optional<vinc::Error> error;
    std::chrono::system_clock::time_point wall_received = std::chrono::system_clock::now();
};

struct Loss {
    std::uint64_t bytes = 0;
    std::uint64_t events = 0;
};

struct SourceLoss {
    std::shared_ptr<const Source> source;
    Loss loss;
};
struct Batch {
    std::vector<std::shared_ptr<const Event>> events;
    Loss skipped;
    std::vector<SourceLoss> sources_skipped;
    Loss unattributed;
};

struct QueueLimits {
    std::size_t bytes = config::output_hub_default_queue_bytes;
    std::size_t events = config::output_hub_default_queue_events;
};

struct Limits {
    std::size_t max_captures = config::output_hub_default_captures;
    std::size_t max_subscribers = config::output_hub_default_subscribers;
    std::size_t total_queue_bytes = config::output_hub_default_total_queue_bytes;
    std::size_t total_queue_events = config::output_hub_default_total_queue_events;
    std::chrono::milliseconds drain_timeout{config::output_hub_default_drain_timeout};
};

struct Stats {
    std::uint64_t received_bytes = 0;
    std::uint64_t truncated_streams = 0;
    std::uint64_t failed_streams = 0;
    std::size_t active_captures = 0;
};

namespace detail {
struct Queue;
#ifdef VINC_OUTPUT_HUB_TESTING
struct OutputHubTestAccess;
#endif
}

// take() and set_enabled() synchronize the queue internally. Move, close and
// destruction require caller synchronization with operations on this Subscription.
// Poll from the destination's own thread. Hub never calls destination code.
// take() transfers shared immutable events; retained batches are caller memory.
class Subscription {
public:
    Subscription() = default;
    Subscription(Subscription&&) noexcept = default;
    Subscription& operator=(Subscription&&) noexcept = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Batch take(std::size_t max_events = config::output_hub_default_take_events);
    void set_enabled(bool enabled); // disabling clears this queue; capture keeps draining

    void close() noexcept {
        queue_.reset();
    }

private:
    friend class OutputHub;

    explicit Subscription(std::shared_ptr<detail::Queue> queue) : queue_(std::move(queue)) {
    }

    std::shared_ptr<detail::Queue> queue_;
};

struct CapturedProcess {
    osal::Process process;
    std::uint64_t capture = 0;
};

class OutputHub {
public:
    static vinc::Result<std::unique_ptr<OutputHub>> create(Limits limits = {});
    ~OutputHub(); // closes readers and joins collection; never kills processes
    OutputHub(const OutputHub&) = delete;
    OutputHub& operator=(const OutputHub&) = delete;
    vinc::Result<Subscription> subscribe(QueueLimits limits = {});
    // Reserves two source identities and buffers before spawn, then captures
    // both outputs regardless of options' output modes. stdin is unchanged.
    vinc::Result<CapturedProcess> spawn(osal::ProcessService& processes, const osal::SpawnOptions& options,
                                        std::string name, std::uint64_t generation);
    void exited(std::uint64_t capture); // start bounded tail drain after observed OS exit
    // Change labels for future events only; already delivered Source objects stay immutable.
    void set_name(std::uint64_t capture, const std::string& name);
    vinc::Result<void> wait_until(vinc::Deadline deadline); // current captures only; does not close admission
    Stats stats() const;
    // First PipeService infrastructure failure, independent of event retention.
    // Historical diagnostics do not change wait_until's drain-completion contract.
    std::optional<vinc::Error> service_error() const;

private:
#ifdef VINC_OUTPUT_HUB_TESTING
    friend struct detail::OutputHubTestAccess;
#endif
    struct Impl;
    explicit OutputHub(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> impl_;
};
}

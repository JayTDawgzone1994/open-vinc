#pragma once
#include <vinc/bag/error.hpp>
#include <vinc/message/generated.hpp>
#include <vinc/config.hpp>
#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace vinc::bag {
struct Options {
    std::uint64_t queue_bytes = config::bag_queue_bytes;
    std::size_t queue_records = config::bag_queue_records;
    std::uint64_t message_bytes = config::bag_message_bytes;
    // Main database file limit, including metadata/indexes; excludes journal files.
    std::uint64_t database_bytes = config::bag_database_bytes;
};

struct Topic {
    std::string name;
    std::string type;
    std::uint64_t schema = 0;
    bool supported = true;
};

struct Counters {
    std::uint64_t received = 0;
    std::uint64_t written = 0;
    std::uint64_t queue_dropped = 0;
    std::uint64_t oversized = 0;
    std::uint64_t bus_lost = 0;
    friend bool operator==(const Counters&, const Counters&) = default;
};

struct Status {
    Counters counters;
    std::uint64_t pending_bytes = 0;
    std::size_t pending_records = 0;
    bool accepting = false;
    bool finished = false;
    std::optional<Error> failure;
};

struct TopicSummary {
    Topic topic;
    std::uint64_t records = 0;
    // Minimum/maximum Unix timestamps, not insertion order when the clock changes.
    std::uint64_t first_timestamp = 0;
    std::uint64_t last_timestamp = 0;
};

struct SourceSummary {
    std::string publisher;
    std::uint64_t publisher_id = 0;
    std::uint64_t records = 0;
};

struct Info {
    bool completed = false;
    std::uint64_t start_utc_ns = 0;
    Counters counters;
    std::vector<TopicSummary> topics;
    std::vector<SourceSummary> sources;
};

// Read-only metadata query; no type decoder or payload allocation is required.
Result<Info> inspect(const std::filesystem::path& directory);

// One bounded queue and one disk worker. enqueue never waits for disk I/O.
// finish stops admission; wait joins. The ModuleHost enforces process stop deadlines.
class Recorder {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Recorder(std::unique_ptr<Impl>);

public:
    using Encoder = Result<message::Snapshot> (*)(const BaseMessage&, std::uint64_t);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    static Result<std::unique_ptr<Recorder>> create(const std::filesystem::path&, Options = {});
    Result<void> discover(Topic);
    Result<void> enqueue(std::unique_ptr<BaseMessage>, std::uint64_t retained_bytes, Encoder);
    void observe_losses(std::uint64_t overwritten, std::uint64_t oversized);
    void finish(std::optional<Error> failure = {});
    Result<void> wait();
    Status status() const;
};
}

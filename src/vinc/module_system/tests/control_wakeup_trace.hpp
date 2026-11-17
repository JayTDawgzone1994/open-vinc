#pragma once

#include "control_wakeup_hooks.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace vinc::test::control_wakeup {
using Clock = std::chrono::steady_clock;
// Includes startup, 64 subscriptions (two requests each), probes and cleanup.
inline constexpr std::size_t trace_capacity = 8192;
struct Sample {
    Point point;
    control::Role role;
    std::uint64_t generation;
    control::Kind kind;
    std::uint16_t domain;
    std::uint16_t operation;
    std::uint64_t request;
    std::int64_t nanoseconds;
};

class Trace {
public:
    void record(Point point, control::Role role, std::uint64_t generation,
                const control::Frame& frame) {
        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count();
        std::lock_guard lock(mutex_);
        if (size_ == samples_.size()) {
            overflow_ = true;
            return;
        }

        // Fixed storage avoids allocations or disk I/O under the Peer mutex.
        samples_[size_++] = {point, role, generation, frame.kind, frame.domain,
                            frame.operation, frame.request, timestamp};
    }

    std::vector<Sample> snapshot() {
        std::lock_guard lock(mutex_);
        if (overflow_) {
            throw std::runtime_error("control wakeup trace capacity exceeded");
        }

        return {samples_.begin(), samples_.begin() + size_};
    }

    void write(const std::filesystem::path& path) {
        const auto copy = snapshot();
        std::ofstream output(path);
        output << "point,role,generation,kind,domain,operation,request,nanoseconds\n";
        for (const auto& sample : copy) {
            output << static_cast<int>(sample.point) << ',' << static_cast<int>(sample.role) << ','
                   << sample.generation << ',' << static_cast<int>(sample.kind) << ','
                   << sample.domain << ',' << sample.operation << ',' << sample.request << ','
                   << sample.nanoseconds << '\n';
        }
        output.close();
        if (!output) {
            throw std::runtime_error("cannot write control wakeup trace");
        }
    }

private:
    std::mutex mutex_;
    std::array<Sample, trace_capacity> samples_{};
    std::size_t size_ = 0;
    bool overflow_ = false;
};
inline Trace trace;
}

// Opt-in measurement fixture; never linked into a normal App or run by CTest.
#include <vinc/vinc.hpp>
#include <vinc_messages/PumpSample.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto pool_warmup = 500ms;
constexpr auto idle_window = 1000ms;
constexpr auto completion_timeout = 30s;
constexpr std::uint64_t slot_bytes = 1024;
constexpr std::size_t max_topics = 4;
constexpr std::size_t max_samples = 4096;
constexpr std::uint32_t max_iterations = 1000000;

template<class T>
auto take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count());
}

struct Usage {
    double cpu_ms;
    std::uint64_t peak_rss_bytes;
};

Usage usage() {
#ifdef _WIN32
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)
        || !GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        throw std::runtime_error("process accounting failed");
    }
    const auto ticks = [](FILETIME value) {
        return (std::uint64_t{value.dwHighDateTime} << 32) | value.dwLowDateTime;
    };
    return {static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0,
        static_cast<std::uint64_t>(memory.PeakWorkingSetSize)};
#else
    rusage measured{};
    if (getrusage(RUSAGE_SELF, &measured) != 0) {
        throw std::runtime_error("process accounting failed");
    }
    const double cpu_ms = 1000.0 * (measured.ru_utime.tv_sec + measured.ru_stime.tv_sec)
        + (measured.ru_utime.tv_usec + measured.ru_stime.tv_usec) / 1000.0;
#ifdef __APPLE__
    const std::uint64_t rss_unit = 1;
#else
    const std::uint64_t rss_unit = 1024;
#endif
    return {cpu_ms, static_cast<std::uint64_t>(measured.ru_maxrss) * rss_unit};
#endif
}
}

class PumpCost final : public vinc::BaseModule<vinc::RunMode::Manual> {
    using Sample = vinc::PumpSample;
    struct Topic {
        vinc::message_bus::Publisher<Sample> publisher;
        vinc::message_bus::Subscriber<Sample> subscriber;
        std::vector<double> latencies_us;
        std::uint64_t checksum = 0;
    };
    std::vector<Topic> topics_;
    std::uint32_t workers_ = 0;
    std::uint32_t iterations_ = 0;
    std::size_t samples_ = 0;
    std::filesystem::path output_;
    std::atomic<std::size_t> completed_ = 0;
    std::mutex completion_mutex_;
    std::condition_variable completed_changed_;
    std::thread pump_;
    vinc::Result<void> pump_result_;

    void receive(std::size_t index, std::unique_ptr<Sample> message) {
        auto& topic = topics_[index];
        const auto sequence = message->data().sequence.get();
        if (sequence != topic.latencies_us.size() || sequence >= samples_) {
            throw std::runtime_error("delivery order/loss mismatch");
        }
        // Observable deterministic CPU work, not sleep or an optimized-away loop.
        std::uint64_t value = sequence + 1;
        for (std::uint32_t step = 0; step < iterations_; ++step) {
            value ^= value << 13;
            value ^= value >> 7;
            value ^= value << 17;
        }
        topic.checksum ^= value;
        topic.latencies_us.push_back(static_cast<double>(now_ns() - message->data().sent_ns.get()) / 1000.0);
        // Per-Topic storage has one writer. The final release sequence publishes
        // every completed callback's records to the waiting measurement thread.
        const auto finished = completed_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (finished == samples_ * topics_.size()) {
            std::lock_guard lock(completion_mutex_);
            completed_changed_.notify_all();
        }
    }

    void write_result(double elapsed_ms, double cpu_ms, double idle_ms, double idle_cpu_ms,
                      std::uint64_t peak_before, std::uint64_t peak_after) {
        std::vector<double> latencies;
        std::uint64_t lost = 0;
        std::uint64_t collisions = 0;
        std::uint64_t checksum = 0;
        for (const auto& topic : topics_) {
            latencies.insert(latencies.end(), topic.latencies_us.begin(), topic.latencies_us.end());
            lost += topic.subscriber.stats().lost_messages;
            collisions += topic.publisher.stats().collisions;
            checksum ^= topic.checksum;
        }
        std::sort(latencies.begin(), latencies.end());
        const auto quantile = [&latencies](std::size_t percentile) {
            const auto rank = (latencies.size() * percentile + 99) / 100;
            return latencies[rank - 1];
        };
        auto temporary = output_;
        temporary += ".partial";
        {
            std::ofstream stream(temporary);
            stream << "workers,topics,samples_per_topic,iterations,delivered,lost,collisions,wall_ms,cpu_ms,"
                "throughput_per_s,p50_us,p99_us,idle_wall_ms,idle_cpu_ms,peak_before_bytes,peak_after_bytes,checksum\n";
            stream << workers_ << ',' << topics_.size() << ',' << samples_ << ',' << iterations_ << ','
                << latencies.size() << ',' << lost << ',' << collisions << ',' << elapsed_ms << ',' << cpu_ms << ','
                << latencies.size() * 1000.0 / elapsed_ms << ',' << quantile(50) << ',' << quantile(99) << ','
                << idle_ms << ',' << idle_cpu_ms << ',' << peak_before << ',' << peak_after << ',' << checksum << '\n';
            stream.flush();
            if (!stream) {
                throw std::runtime_error("result write failed");
            }
        }
        // The Tcl controller observes only complete output, then requests normal exit.
        std::filesystem::rename(temporary, output_);
    }

public:
    PumpCost() : BaseModule("pumpCost") {
    }

    vinc::Result<void> on_start(const std::vector<std::string>& args) override {
        if (args.size() != 6) {
            throw std::runtime_error("expected workers topics iterations samples output");
        }
        workers_ = static_cast<std::uint32_t>(std::stoul(args[1]));
        const auto count = std::stoul(args[2]);
        iterations_ = static_cast<std::uint32_t>(std::stoul(args[3]));
        samples_ = std::stoul(args[4]);
        output_ = take(vinc::osal::path_from_utf8(args[5]));
        if (count == 0 || count > max_topics || samples_ == 0 || samples_ > max_samples
            || iterations_ > max_iterations || workers_ > 4) {
            throw std::runtime_error("measurement bounds exceeded");
        }
        topics_.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto name = "cost/topic" + std::to_string(index);
            auto publisher = take(context.advertise<Sample>(name,
                {.slots = static_cast<std::uint32_t>(samples_), .max_message_bytes = slot_bytes}));
            auto subscriber = take(context.subscribe<Sample>(name, [this, index](std::unique_ptr<Sample> message) {
                receive(index, std::move(message));
            }));
            Topic topic{std::move(publisher), std::move(subscriber), {}, 0};
            topic.latencies_us.reserve(samples_);
            topics_.push_back(std::move(topic));
        }
        return {};
    }

    vinc::Result<void> main() override {
        pump_ = std::thread([this] { pump_result_ = context.pumpLoop({.topic_workers = workers_}); });
        // Warmup is measurement settling time, not synchronization or a test oracle.
        std::this_thread::sleep_for(pool_warmup);
        const auto idle_begin = Clock::now();
        const auto before_idle = usage();
        std::this_thread::sleep_for(idle_window);
        const auto before_load = usage();
        const auto started = Clock::now();
        auto message = vinc::make_message<Sample>();
        for (std::size_t sequence = 0; sequence < samples_; ++sequence) {
            for (auto& topic : topics_) {
                message->data().sequence = sequence;
                message->data().sent_ns = now_ns();
                take(topic.publisher.push(*message));
            }
        }
        {
            std::unique_lock lock(completion_mutex_);
            if (!completed_changed_.wait_until(lock, started + completion_timeout, [this] {
                return completed_.load(std::memory_order_acquire) == samples_ * topics_.size();
            })) {
                throw std::runtime_error("measurement completion timed out");
            }
        }
        const auto ended = Clock::now();
        const auto after_load = usage();
        write_result(std::chrono::duration<double, std::milli>(ended - started).count(),
            after_load.cpu_ms - before_load.cpu_ms,
            std::chrono::duration<double, std::milli>(started - idle_begin).count(),
            before_load.cpu_ms - before_idle.cpu_ms,
            before_load.peak_rss_bytes, after_load.peak_rss_bytes);
        return {};
    }

    vinc::Result<void> on_stop() override {
        // Framework quiescence closes pump admission before joining this owned
        // thread. It is never detached and never needs on_stop to finish a callback.
        if (pump_.joinable()) {
            pump_.join();
        }
        return pump_result_;
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
        return 0;
    }
};
REGISTER_MODULE(PumpCost, "PumpCost")

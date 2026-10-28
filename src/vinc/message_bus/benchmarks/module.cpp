#include <benchmark.hpp>
#include <vinc/module/base_module.hpp>
#include <vinc_messages/BenchmarkPayload.hpp>
#include <optional>
#include <thread>

namespace {
using namespace vinc;
using namespace benchmark;

struct Sent {
    std::uint64_t sequence;
    std::uint64_t prepare_ns;
    std::uint64_t begin_ns;
    std::uint64_t end_ns;
};

struct Received {
    std::uint64_t sequence;
    std::uint64_t arrival_ns;
    bool valid;
};

class BenchmarkModule : public BaseModule<RunMode::Manual> {
    Options options_;
    std::optional<message_bus::Publisher<BenchmarkPayload>> publisher_;
    std::optional<message_bus::Subscriber<BenchmarkPayload>> subscriber_;
    std::unique_ptr<BenchmarkPayload> local_;
    std::vector<Sent> sent_;
    std::vector<Received> received_;
    std::uint64_t image_bytes_ = 0;
    std::uint64_t excess_callbacks_ = 0;
    bool go_ = false;
    bool done_ = false;
    Resources resources_before_;
    std::uint64_t resource_start_ = 0;

    Result<void> fill(BenchmarkPayload& message, std::uint64_t sequence) {
        auto resized = message.data().payload.resize(options_.bytes);
        if (!resized) {
            return std::unexpected(resized.error());
        }

        // resize initializes the entire byte array. Boundary markers catch wrong
        // sizes/slots without adding a full application scan to every callback.
        message.data().payload[0] = first_marker;
        message.data().payload[options_.bytes - 1] = last_marker;
        message.data().sequence = sequence;
        return {};
    }

    Result<void> create_publisher() {
        local_ = make_message<BenchmarkPayload>();
        auto filled = fill(*local_, 0);
        if (!filled) {
            return filled;
        }

        auto usage = local_->usage();
        if (!usage) {
            return std::unexpected(usage.error());
        }

        image_bytes_ = usage->compact_bytes;
        auto published = context.advertise<BenchmarkPayload>("benchmark/payload", {
            .slots = static_cast<std::uint32_t>(options_.slots),
            .max_message_bytes = options_.bytes + image_allowance
        });
        if (!published) {
            return std::unexpected(published.error());
        }

        publisher_.emplace(std::move(*published));
        if (options_.mode == "loan") {
            local_.reset();
        }

        sent_.reserve(static_cast<std::size_t>(options_.samples + options_.warmup));
        return {};
    }

    Result<void> create_subscriber() {
        received_.reserve(static_cast<std::size_t>(options_.samples + options_.warmup));
        auto subscribed = context.subscribe<BenchmarkPayload>("benchmark/payload",
            [this](std::unique_ptr<BenchmarkPayload> message) {
                // Timestamp the callback entry before application validation or storage.
                const auto arrival = now_ns();
                const auto sequence = message->data().sequence.get();
                const auto& payload = std::as_const(*message).data().payload;
                bool valid = payload.size() == options_.bytes;
                if (valid) {
                    valid = payload[0] == first_marker && payload[options_.bytes - 1] == last_marker;
                }

                if (received_.size() < options_.samples + options_.warmup) {
                    received_.push_back({sequence, arrival, valid});
                } else {
                    ++excess_callbacks_;
                }
            });
        if (!subscribed) {
            return std::unexpected(subscribed.error());
        }

        subscriber_.emplace(std::move(*subscribed));
        return {};
    }

    Result<void> send_one(std::uint64_t sequence) {
        const auto prepare = now_ns();
        if (options_.mode == "push") {
            local_->data().sequence = sequence;
            const auto begin = now_ns();
            auto sent = publisher_->push(*local_);
            const auto end = now_ns();
            if (!sent) {
                return sent;
            }

            sent_.push_back({sequence, begin - prepare, begin, end});
        } else {
            auto loan = publisher_->get_next();
            if (!loan) {
                return std::unexpected(loan.error());
            }

            auto filled = fill(**loan, sequence);
            if (!filled) {
                return filled;
            }

            const auto begin = now_ns();
            auto committed = publisher_->commit(*loan);
            const auto end = now_ns();
            if (!committed) {
                return committed;
            }

            sent_.push_back({sequence, begin - prepare, begin, end});
        }

        return {};
    }

    Result<void> send_all() {
        auto next = std::chrono::steady_clock::now();
        const auto period = std::chrono::nanoseconds(options_.period_ns);
        for (std::uint64_t sequence = 1; sequence <= options_.warmup + options_.samples; ++sequence) {
            if (!context.living()) {
                return std::unexpected(failure());
            }

            if (sequence == options_.warmup + 1) {
                std::this_thread::sleep_for(warmup_drain);
                next = std::chrono::steady_clock::now();
            }

            std::this_thread::sleep_until(next);
            const auto started = std::chrono::steady_clock::now();
            auto sent = send_one(sequence);
            if (!sent) {
                return sent;
            }

            // Never catch up by bursting overdue samples. The report compares
            // requested and achieved rates so overload is visible.
            next = started + period;
        }

        return save_publisher();
    }

    Result<void> save_publisher() {
        const auto stats = publisher_->stats();
        const auto end = now_ns();
        const auto resources_after = resources();
        std::ofstream data(options_.directory / "sent.csv");
        data << "sequence,prepare_ns,begin_ns,end_ns\n";
        for (const auto& sample : sent_) {
            data << sample.sequence << ',' << sample.prepare_ns << ',' << sample.begin_ns << ',' << sample.end_ns << '\n';
        }
        data.close();

        std::ofstream stats_file(options_.directory / "publisher.csv");
        stats_file << "shared_bytes,image_bytes,slots,published,collisions,total_wait_ns,longest_wait_ns\n";
        stats_file << stats.shared_bytes << ',' << image_bytes_ << ',' << stats.effective_slots << ','
                   << stats.published << ',' << stats.collisions << ',' << stats.total_wait_ns << ','
                   << stats.longest_wait_ns << '\n';
        stats_file.close();
        const auto resources_saved = save_resources(options_.directory / "publisher_resources.csv",
            resources_before_, resources_after, resource_start_, end);
        if (data.fail() || stats_file.fail() || !resources_saved) {
            return std::unexpected(failure());
        }

        // Keep the Publisher alive until the central driver drains the subscriber.
        // Closing it here would legitimately remove unread history.
        done_ = true;
        return {};
    }

    Result<void> save_subscriber() {
        const auto end = now_ns();
        const auto resources_after = resources();
        const auto stats = subscriber_->stats();
        std::ofstream data(options_.directory / "received.csv");
        data << "sequence,arrival_ns,valid\n";
        for (const auto& sample : received_) {
            data << sample.sequence << ',' << sample.arrival_ns << ',' << sample.valid << '\n';
        }
        data.close();

        std::ofstream stats_file(options_.directory / "subscriber.csv");
        stats_file << "lost_messages,overwrite_events,excess_callbacks\n";
        stats_file << stats.lost_messages << ',' << stats.overwrite_events << ',' << excess_callbacks_ << '\n';
        stats_file.close();
        const auto resources_saved = save_resources(options_.directory / "subscriber_resources.csv",
            resources_before_, resources_after, resource_start_, end);
        if (data.fail() || stats_file.fail() || !resources_saved) {
            return std::unexpected(failure());
        }

        return {};
    }

public:
    BenchmarkModule() : BaseModule("benchmarkNode") {
    }

    Result<std::string> on_name(std::vector<std::string>& args) override {
        std::string requested_name;
        if (args.size() == 10 && args[8] == "--name") {
            requested_name = args[9];
            args.resize(8);
        }
        if (!parse(args, options_)) {
            return std::unexpected(failure());
        }

        if (!requested_name.empty()) {
            return requested_name;
        }
        return "benchmark_" + options_.role;
    }

    Result<void> on_start(const std::vector<std::string>&) override {
        resources_before_ = resources();
        resource_start_ = now_ns();
        if (options_.role == "publisher") {
            return create_publisher();
        }

        return create_subscriber();
    }

    Result<void> main() override {
        if (options_.role == "subscriber") {
            return context.pumpLoop();
        }

        while (context.living() && !go_) {
            auto pumped = context.pumpOnce();
            if (!pumped) {
                return std::unexpected(pumped.error());
            }

            std::this_thread::sleep_for(control_poll);
        }

        if (!context.living()) {
            return {};
        }

        auto sent = send_all();
        if (!sent) {
            return sent;
        }

        return context.pumpLoop();
    }

    Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        if (args.size() != 1) {
            return std::unexpected(failure());
        }

        if (args[0] == "go" && publisher_) {
            go_ = true;
            return 0;
        }

        if (args[0] == "count" && subscriber_) {
            return static_cast<std::int32_t>(received_.size());
        }

        if (args[0] == "save" && subscriber_) {
            auto saved = save_subscriber();
            if (!saved) {
                return std::unexpected(saved.error());
            }

            return 0;
        }

        if (args[0] == "done" && publisher_) {
            return done_ ? 1 : 0;
        }

        return std::unexpected(failure());
    }
};
}

REGISTER_MODULE(BenchmarkModule, "busBenchmark")

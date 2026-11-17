#include "bus_test_hooks.hpp"
#include <vinc/control/channel.hpp>
#include <vinc/message_bus/detail/bus_manager.hpp>
#include <vinc/message_bus/detail/topology_protocol.hpp>
#include <vinc_messages/Frame.hpp>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace vinc::message_bus::detail::testing {
namespace {
using namespace std::chrono_literals;
constexpr auto operation_budget = 5s;
constexpr auto dispatcher_tick = 20ms;
// Longer than heartbeat expiry: a live paused dispatcher must not expire.
constexpr auto idle_observation = 2200ms;
constexpr auto heartbeat_period = 50ms;
constexpr auto heartbeat_expiry = 2s;
constexpr std::size_t shared_topic_readers = 128;
constexpr std::size_t snapshot_topic_count = 160;
constexpr PublisherOptions small_ring{2, 1024};
std::atomic<std::size_t> peak_history{0};
std::atomic<std::size_t> peak_pages{0};
std::atomic<std::size_t> peak_saved{0};
std::atomic<std::uint64_t> worker_wakes{0};

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

template<class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    return std::move(*result);
}

void accept(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

Deadline deadline() {
    return control::Clock::now() + operation_budget;
}

void maximum(std::atomic<std::size_t>& value, std::size_t candidate) noexcept {
    auto previous = value.load();
    while (previous < candidate && !value.compare_exchange_weak(previous, candidate)) {
    }
}

// Real sockets, heartbeat, Bus worker and Client; only the Host dispatcher is
// controllably paused. Its bridge threads are test drivers, not runtime workers.
class Host {
    BusManager& manager_;
    std::uint64_t owner_;
    std::unique_ptr<control::Service> service_;
    std::shared_ptr<control::Session> central_;
    std::shared_ptr<control::Session> module_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool paused_ = false;
    bool parked_ = false;
    std::string failure_;
    std::jthread requests_;
    std::jthread dispatcher_;

    template<class Work> void guarded(Work work) {
        try {
            work();
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            failure_ = error.what();
            cv_.notify_all();
        }
    }

    void dispatch(std::stop_token stop) {
        std::uint64_t transfer = 0;
        std::size_t staged = 0;
        while (!stop.stop_requested()) {
            {
                std::unique_lock lock(mutex_);
                parked_ = paused_;
                cv_.notify_all();
                cv_.wait(lock, [&] { return !paused_ || stop.stop_requested(); });
                parked_ = false;
            }
            if (stop.stop_requested()) {
                break;
            }

            accept(RuntimeAccess::progress_control(*client));
            auto incoming = module_->receive_until(control::Clock::now() + dispatcher_tick);
            if (!incoming && incoming.error().is(control::ErrorCode::Timeout)) {
                continue;
            }
            auto frame = take(std::move(incoming));
            if (frame.operation == SyncPage) {
                auto page = take(decode_topology_page(frame.payload));
                ++pages;
                wire_bytes += frame.payload.size() + control::header_size;
                maximum(peak_page_wire, frame.payload.size() + control::header_size);
                if (transfer != page.id.transfer) {
                    transfer = page.id.transfer;
                    staged = 0;
                }
                staged += page.records.size();
                maximum(peak_staged_wire, staged);
                if (page.mode == TopologyMode::Snapshot) {
                    ++snapshot_pages;
                }
                // Compare identical observed deltas with the old one-frame-per-
                // reader encoding. This is a wire-size model, not a v5 runtime run.
                if (page.mode == TopologyMode::Delta) {
                    accept(visit_topology_records(page.records, [&](auto kind, auto body) -> Result<void> {
                        if (kind == Attach) {
                            auto group = take(decode_source_group(body));
                            control::Bytes descriptor;
                            accept(encode_descriptor(descriptor, group.publisher));
                            constexpr std::size_t old_reader_fields = 8 + 4 + 8;
                            v5_equivalent_frames += group.readers.size();
                            v5_equivalent_bytes += group.readers.size() *
                                (control::header_size + descriptor.size() + old_reader_fields);
                        } else if (kind == Detach) {
                            control::Reader reader(body);
                            (void)reader.u64();
                            const auto count = reader.u32();
                            constexpr std::size_t old_detach_fields = 8 + 8;
                            v5_equivalent_frames += count;
                            v5_equivalent_bytes += count * (control::header_size + old_detach_fields);
                        }
                        return {};
                    }));
                }
            }
            accept(RuntimeAccess::receive(*client, std::move(frame)));
        }
    }

public:
    std::shared_ptr<Client> client;
    std::atomic<std::size_t> pages{0};
    std::atomic<std::size_t> snapshot_pages{0};
    std::atomic<std::size_t> wire_bytes{0};
    std::atomic<std::size_t> peak_page_wire{0};
    std::atomic<std::size_t> peak_staged_wire{0};
    std::atomic<std::size_t> ack_frames{0};
    std::atomic<std::size_t> ack_wire_bytes{0};
    std::atomic<std::size_t> v5_equivalent_frames{0};
    std::atomic<std::size_t> v5_equivalent_bytes{0};

    Host(BusManager& manager, std::uint64_t owner) : manager_(manager), owner_(owner) {
        service_ = take(control::Service::create(2));
        auto listener = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
        accept(listener.bind(osal::IpEndpoint{osal::IPv4Address::loopback(), 0}));
        accept(listener.listen(1));
        auto socket = take(osal::Socket::open(osal::AddressFamily::IPv4, osal::SocketType::Stream));
        accept(socket.connect_until(take(listener.local_address()), deadline()));
        auto accepted = take(listener.accept_until(deadline()));
        control::Options options;
        options.role = control::Role::Central;
        options.generation = owner;
        options.token = "topology-load";
        options.heartbeat_interval = heartbeat_period;
        options.heartbeat_timeout = heartbeat_expiry;
        central_ = take(service_->attach(std::move(accepted), options));
        options.role = control::Role::Module;
        module_ = take(service_->attach(std::move(socket), options));
        accept(central_->wait_open(deadline()));
        accept(module_->wait_open(deadline()));
        client = take(RuntimeAccess::create(module_, owner));
        requests_ = std::jthread([this](std::stop_token stop) {
            guarded([&] {
                while (!stop.stop_requested()) {
                    auto incoming = central_->receive_until(control::Clock::now() + dispatcher_tick);
                    if (!incoming && incoming.error().is(control::ErrorCode::Timeout)) {
                        continue;
                    }
                    auto frame = take(std::move(incoming));
                    if (frame.operation == SyncAck) {
                        ++ack_frames;
                        ack_wire_bytes += frame.payload.size() + control::header_size;
                    }
                    accept(manager_.submit(owner_, "loadFixture", central_, std::move(frame)));
                }
            });
        });
        dispatcher_ = std::jthread([this](std::stop_token stop) {
            guarded([&] { dispatch(stop); });
        });
    }

    ~Host() {
        stop();
    }

    void stop() {
        requests_.request_stop();
        dispatcher_.request_stop();
        cv_.notify_all();
        if (requests_.joinable()) {
            requests_.join();
        }
        if (dispatcher_.joinable()) {
            dispatcher_.join();
        }
        RuntimeAccess::shutdown(*client);
    }

    void healthy() {
        std::lock_guard lock(mutex_);
        if (!failure_.empty()) {
            throw std::runtime_error(failure_);
        }
        require(module_->state() == control::State::Open, "heartbeat/session lost during topology lag");
    }

    void pause() {
        std::unique_lock lock(mutex_);
        paused_ = true;
        require(cv_.wait_until(lock, deadline(), [&] { return parked_ || !failure_.empty(); }),
                "dispatcher did not enter its pause gate");
        require(failure_.empty(), "dispatcher failed before pause");
    }

    void resume() {
        std::lock_guard lock(mutex_);
        paused_ = false;
        cv_.notify_all();
    }

    void converge(std::size_t relationships) {
        const auto end = deadline();
        while (control::Clock::now() < end) {
            healthy();
            const auto state = manager_.topology_status(owner_);
            if (state && state->applied == state->target &&
                RuntimeAccess::inspect_attachments(*client).size() == relationships) {
                return;
            }
            std::this_thread::yield();
        }
        throw std::runtime_error("topology did not converge without subsequent registration");
    }
};

void run_load(std::size_t publishers, bool distinct_topics, bool owner_exit) {
    peak_history = 0;
    peak_pages = 0;
    peak_saved = 0;
    auto manager = take(BusManager::create());
    Host writer(*manager, 1);
    Host slow(*manager, 2);
    Host fast(*manager, 3);
    std::vector<Subscriber<Frame>> readers;
    const auto reader_count = distinct_topics ? publishers : shared_topic_readers;
    for (std::size_t index = 0; index < reader_count; ++index) {
        const auto topic = distinct_topics ? "load/" + std::to_string(index) : "load/shared";
        readers.push_back(take(slow.client->subscribe<Frame>(topic)));
    }
    auto independent = take(writer.client->advertise<Frame>("independent", small_ring));
    auto independent_reader = take(fast.client->subscribe<Frame>("independent"));
    auto payload = make_message<Frame>();
    std::uint64_t sequence = 0;
    auto exercise_fast = [&] {
        payload->data().sequence = ++sequence;
        accept(independent.push(*payload));
        auto received = take(independent_reader.get_last());
        require(received->data().sequence == sequence, "unrelated Topic stopped communicating");
        take(fast.client->list_topics());
        fast.healthy();
        slow.healthy();
    };

    slow.pause();
    std::vector<Publisher<Frame>> sources;
    for (std::size_t index = 0; index < publishers; ++index) {
        const auto topic = distinct_topics ? "load/" + std::to_string(index) : "load/shared";
        sources.push_back(take(writer.client->advertise<Frame>(topic, small_ring)));
        exercise_fast();
        payload->data().sequence = index + 1;
        accept(sources.back().push(*payload));
    }
    auto lagging = manager->topology_status(2);
    require(lagging && lagging->applied < lagging->target, "paused Host was not marked lagging");
    require(RuntimeAccess::inspect_attachments(*slow.client).empty(), "paused Host applied a partial update");
    accept(manager->drain_until(deadline()));
    const auto idle_start = worker_wakes.load();
    std::this_thread::sleep_for(idle_observation);
    const auto idle_wakes = worker_wakes.load() - idle_start;
    require(idle_wakes == 0, "waiting for a topology ACK polls the Bus worker");
    exercise_fast();

    const auto resumed = control::Clock::now();
    slow.resume();
    slow.converge(distinct_topics ? publishers : publishers * reader_count);
    const auto convergence_us = std::chrono::duration_cast<std::chrono::microseconds>(
        control::Clock::now() - resumed).count();
    // Every lease retains its index. Unrelated Topic descriptors never enter this Host.
    std::set<std::pair<std::uint64_t, std::uint32_t>> leases;
    for (const auto& attachment : RuntimeAccess::inspect_attachments(*slow.client)) {
        require(attachment.descriptor.id != 0 && attachment.floor == 0, "invalid source identity or floor");
        require(leases.emplace(attachment.descriptor.id, attachment.reader).second, "duplicate reader lease");
    }
    require(RuntimeAccess::inspect_attachments(*fast.client).size() == 1, "unrelated topology leaked to fast Host");
    for (std::size_t index = 0; index < readers.size(); ++index) {
        const auto expected = distinct_topics ? index + 1 : publishers;
        require(take(readers[index].get_last())->data().sequence == expected,
                "converged relationship cannot read the retained payload");
    }
    if (distinct_topics) {
        require(slow.snapshot_pages > 0, "history overflow did not exercise snapshot transfer");
    }

    slow.pause();
    if (owner_exit) {
        // Model the confirmed-exit handoff only after this fixture stops all access.
        // Native process death is independently covered by message_bus.module.
        writer.stop();
        manager->exited(1);
        accept(manager->drain_until(deadline()));
        take(fast.client->list_topics());
    } else {
        sources.clear();
        // Request admission waits for the prior destructor release batches, not topology ACKs.
        take(writer.client->list_topics());
        exercise_fast();
    }
    lagging = manager->topology_status(2);
    require(lagging && lagging->applied < lagging->target, "close waited for the paused observer");
    slow.resume();
    slow.converge(0);
    slow.healthy();
    fast.healthy();
    require(peak_history <= config::message_bus_topology_total_history_bytes,
            "encoded history exceeded the global budget");
    if (!distinct_topics) {
        require(slow.v5_equivalent_frames == 2 * publishers * reader_count,
                "wire comparison must include every Attach and Detach lease");
    }
    std::cout << "topology-load publishers=" << publishers << " readers=" << reader_count
              << " distinct=" << distinct_topics << " owner_exit=" << owner_exit
              << " pages=" << slow.pages << " wire_bytes=" << slow.wire_bytes
              << " largest_page_wire=" << slow.peak_page_wire
              << " ack_frames=" << slow.ack_frames << " ack_wire_bytes=" << slow.ack_wire_bytes
              << " v5_delta_frame_model=" << slow.v5_equivalent_frames
              << " v5_delta_byte_model=" << slow.v5_equivalent_bytes
              << " snapshots=" << slow.snapshot_pages << " staged_wire_peak=" << slow.peak_staged_wire
              << " history_sample_peak=" << peak_history << " pages_sample_peak=" << peak_pages
              << " saved_sample_peak=" << peak_saved << " convergence_us=" << convergence_us
              << " idle_worker_wakes=" << idle_wakes << '\n';
}
} // namespace

void topology_storage(std::size_t history, std::size_t pages, std::size_t saved) noexcept {
    maximum(peak_history, history);
    maximum(peak_pages, pages);
    maximum(peak_saved, saved);
}

void topology_worker_wake() noexcept {
    ++worker_wakes;
}

void run_topology_load_tests() {
    run_load(1, false, false);
    run_load(16, false, false);
    run_load(snapshot_topic_count, true, true);
}

void report_topology_storage() {
    std::cout << "topology-storage history_sample_peak=" << peak_history
              << " pages_sample_peak=" << peak_pages << " saved_sample_peak=" << peak_saved << '\n';
}
} // namespace vinc::message_bus::detail::testing

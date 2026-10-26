// Low-level ring buffer tests for publication, overwrite protection and reader contention.
// Use private APIs deliberately to place readers/writers at protocol boundaries.

#include <vinc/message_bus/detail/ring_buffer.hpp>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vinc/osal/process.hpp>
#include <cstring>
#include <array>
#include <thread>
using namespace vinc::message_bus;
using namespace vinc::message_bus::detail;
using namespace std::chrono_literals;

namespace {
constexpr int counter_children = 4;
constexpr int increments_per_child = 20000;
constexpr std::uint64_t stress_publications = 3000;
constexpr std::size_t test_payload_bytes = 1024;
constexpr unsigned payload_pattern_modulus = 251;
constexpr auto child_cleanup_timeout = 3s;
constexpr auto child_completion_timeout = 15s;
constexpr auto collision_start_timeout = 2s;
constexpr auto protected_copy_probe = 20ms;
constexpr auto stress_timeout = 10s;
}

void check(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

void ok(vinc::Result<void> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
}

template <class T> T os_take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

struct Children {
    std::vector<vinc::osal::Process> processes;

    ~Children() {
        for (auto& process : processes) {
            (void)process.terminate();
            (void)process.wait_until(std::chrono::steady_clock::now() + child_cleanup_timeout);
        }
    }
};

// Exercise the acquisition helpers at both named-resource boundaries. Failure must
// release names created by this attempt without unlinking somebody else's region.
void check_partial_creation_cleanup(Descriptor descriptor) {
    struct UnlinkName {
        std::string name;

        ~UnlinkName() {
            (void)vinc::osal::SharedMemory::unlink_name(name);
        }
    };

    descriptor.control = shared_name('c');
    descriptor.data = shared_name('d');
    auto existing_control = os_take(vinc::osal::SharedMemory::create(
        descriptor.control, RingBuffer::control_bytes(descriptor.slots, descriptor.readers)));
    UnlinkName control_cleanup{descriptor.control};
    auto duplicate_control = RingBuffer::create(descriptor);
    check(!duplicate_control, "existing control name rejects creation");
    check(duplicate_control.error().is(ErrorCode::Io) &&
              duplicate_control.error().is_operation(ErrorOperation::CreateControl) &&
              duplicate_control.error().causes().size() == 1 &&
              duplicate_control.error().causes().front().domain == vinc::ErrorDomain::Osal &&
              bool(duplicate_control.error().native),
          "control creation preserves OSAL cause and native diagnostic");
    auto retained_control =
        vinc::osal::SharedMemory::open(descriptor.control, vinc::osal::SharedMemoryAccess::ReadWrite);
    check(bool(retained_control), "failed creation preserves existing control name");

    descriptor.control = shared_name('c');
    auto existing_payload = os_take(vinc::osal::SharedMemory::create(
        descriptor.data, descriptor.slots * RingBuffer::slot_stride(descriptor.bytes)));
    UnlinkName payload_cleanup{descriptor.data};
    auto duplicate_payload = RingBuffer::create(descriptor);
    check(!duplicate_payload, "existing payload name rejects partial creation");
    check(duplicate_payload.error().is_operation(ErrorOperation::CreatePayload) &&
              duplicate_payload.error().causes().size() == 1,
          "partial creation identifies payload failure without extra wrapping");
    auto released_control =
        vinc::osal::SharedMemory::open(descriptor.control, vinc::osal::SharedMemoryAccess::ReadWrite);
    check(!released_control, "partial creation releases its new control name");
    auto retained_payload =
        vinc::osal::SharedMemory::open(descriptor.data, vinc::osal::SharedMemoryAccess::ReadWrite);
    check(bool(retained_payload), "partial creation preserves existing payload name");
}

int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--counter") {
            auto counter = take(NamedCounter::open(argv[2]));
            auto second =
                os_take(counter->memory.map(sizeof(SharedWord), vinc::osal::SharedMemoryAccess::ReadWrite));
            // Address-layout test only: uintptr_t represents native pointer bits on
            // supported 64-bit hosts. No serialized address is dereferenced.
            const auto parent_address = std::stoull(argv[3]);
            if (reinterpret_cast<std::uintptr_t>(counter->mapping.bytes().data()) == parent_address) {
                counter->mapping = std::move(second);
            }

            check(reinterpret_cast<std::uintptr_t>(counter->mapping.bytes().data()) != parent_address,
                  "IPC mapping address differs");
            check(vinc::osal::monotonic_nanoseconds() >= std::stoull(argv[4]),
                  "same-host monotonic clock across processes");
            for (int i = 0; i < increments_per_child; ++i) {
                (void)take(counter->next());
            }

            return 0;
        }

        check(argc == 2, "expected executable path");
        // Scenario: native atomics and monotonic time across independent mappings.
        auto counter = take(NamedCounter::create(shared_name('t')));
        auto service = os_take(vinc::osal::ProcessService::create());
        Children children;
        for (int i = 0; i < counter_children; ++i) {
            vinc::osal::SpawnOptions options;
            options.executable = std::filesystem::absolute(argv[1]);
            // Transmit the numeric address only to verify that IPC does not depend on
            // equal virtual addresses; the child never treats it as a pointer.
            options.arguments = {
                "--counter", counter->name,
                std::to_string(reinterpret_cast<std::uintptr_t>(counter->mapping.bytes().data())),
                std::to_string(vinc::osal::monotonic_nanoseconds())};
            children.processes.push_back(os_take(service.spawn(options)));
        }

        for (auto& child : children.processes) {
            const auto exit =
                os_take(child.wait_until(std::chrono::steady_clock::now() + child_completion_timeout));
            check(exit.kind == vinc::osal::ExitKind::Code && exit.value == 0, "IPC atomic helper");
        }

        check(take(counter->next()) == counter_children * increments_per_child + 1,
              "IPC counter has no lost updates");
        // Scenario: merge ordering uses timestamps and registration rank, never count.
        Metadata equal_a{1, 1000, 100, 900};
        Metadata equal_b{999, 1, 100, 1};
        check(preferred(equal_a, 1, equal_b, 2, false) && preferred(equal_a, 1, equal_b, 2, true),
              "ties use publisher rank, not counter");
        equal_a.monotonic_timestamp = 99;
        check(preferred(equal_a, 9, equal_b, 1, false) && !preferred(equal_a, 9, equal_b, 1, true),
              "monotonic timestamp, not backward UTC, is the cross-publisher key");
        // Scenario: validate layout before opening the payload.
        Descriptor descriptor;
        descriptor.id = 1;
        descriptor.owner = 1;
        descriptor.schema = 42;
        descriptor.rank = 1;
        descriptor.slots = 2;
        descriptor.readers = 4;
        descriptor.bytes = test_payload_bytes;
        descriptor.control = shared_name('c');
        descriptor.data = shared_name('d');
        auto creator = take(RingBuffer::create(descriptor));
        auto writer = take(RingBuffer::open(descriptor, true));
        auto reader = take(RingBuffer::open(descriptor, false));
        auto invalid = descriptor;
        invalid.schema = 43;
        auto rejected = RingBuffer::open(invalid, false);
        check(!rejected && rejected.error().is(ErrorCode::Protocol),
              "schema mismatch rejected before payload mapping");
        invalid = descriptor;
        invalid.bytes = UINT64_MAX;
        rejected = RingBuffer::open(invalid, false);
        check(!rejected && rejected.error().is(ErrorCode::Protocol), "descriptor size overflow rejected");
        creator->header().version = 1;
        rejected = RingBuffer::open(descriptor, false);
        check(!rejected && rejected.error().is(ErrorCode::Protocol), "old timestamp layout rejected");
        creator->header().version = ring_buffer_layout_version;
        check_partial_creation_cleanup(descriptor);

        // Scenario: an announced reader blocks reuse; stale generations cannot re-enter.
        std::atomic<bool> closed{false};
        auto first = take(writer->reserve(closed));
        first.bytes[0] = std::byte{11};
        ok(writer->publish(first, 64, 1000, 100, 1));
        auto first_metadata = reader->candidate(0, false);
        check(first_metadata && first_metadata->sequence == 1, "first candidate");
        check(first_metadata->timestamp == 1000 && first_metadata->monotonic_timestamp == 100,
              "independent clock fields survive shared memory");
        check(reader->protect(*first_metadata, 0), "protect first generation");
        auto second = take(writer->reserve(closed));
        second.bytes[0] = std::byte{22};
        ok(writer->publish(second, 64, 900, 101, 2));
        auto future = std::async(std::launch::async, [&] {
            return writer->reserve(closed);
        });

        struct Unpin {
            std::shared_ptr<RingBuffer> ring_buffer;

            ~Unpin() {
                ring_buffer->unprotect(0);
            }
        } unpin{reader};

        const auto deadline = std::chrono::steady_clock::now() + collision_start_timeout;
        while (!writer->stats(2).current_wait_ns && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }

        check(writer->stats(2).collisions == 1, "one collision per wait episode");
        check(future.wait_for(protected_copy_probe) == std::future_status::timeout,
              "writer waits while copy protected");
        check(!reader->protect(*first_metadata, 1), "closing blocks new readers");
        check(reader->payload(*first_metadata)[0] == std::byte{11}, "protected data remains intact");
        reader->unprotect(0);
        auto third = take(future.get());
        check(!reader->protect(*first_metadata, 0), "stale generation rejected");
        auto latest = reader->candidate(0, true);
        check(latest && latest->sequence == 2, "latest survives outstanding loan");
        third.bytes[0] = std::byte{33};
        ok(writer->publish(third, 64, 800, 102, 3));
        auto oldest = reader->candidate(0, false);
        check(oldest && oldest->sequence == 2, "slow reader skips overwritten sequence");
        check(writer->stats(2).total_wait_ns > 0 && writer->stats(2).current_wait_ns == 0,
              "wait duration completed");
        creator->retire();
        check(!reader->candidate(0, true), "retired ring buffer is not readable");
        descriptor.control = shared_name('c');
        descriptor.data = shared_name('d');
        // Scenario: repeatedly overwrite while the framework copies protected payloads.
        auto stress = take(RingBuffer::create(descriptor));
        std::atomic<bool> finished{false};
        std::atomic<bool> failed{false};
        std::atomic<bool> cancel{false};
        std::jthread writing_thread([&] {
            for (std::uint64_t sequence = 1; sequence <= stress_publications; ++sequence) {
                auto result = stress->reserve(cancel);
                if (!result) {
                    if (!cancel.load()) {
                        failed.store(true);
                    }
                    break;
                }

                std::memset(result->bytes.data(), static_cast<int>(sequence % payload_pattern_modulus),
                            test_payload_bytes);
                if (!stress->publish(*result, test_payload_bytes, sequence + 1000, sequence, sequence)) {
                    failed.store(true);
                    break;
                }
            }

            finished.store(true);
        });

        struct Release {
            std::shared_ptr<RingBuffer> ring_buffer;
            std::atomic<bool>& cancel;

            ~Release() {
                ring_buffer->unprotect(0);
                cancel.store(true);
            }
        } release{stress, cancel};

        std::uint64_t cursor = 0;

        std::uint64_t copies = 0;
        const auto stress_end = std::chrono::steady_clock::now() + stress_timeout;
        while (!finished.load() || cursor < shared_load(stress->header().head)) {
            check(std::chrono::steady_clock::now() < stress_end, "bounded stress test");
            auto metadata = stress->candidate(cursor, false);
            if (!metadata || !stress->protect(*metadata, 0)) {
                std::this_thread::yield();
                continue;
            }

            check(metadata->timestamp == metadata->sequence + 1000 &&
                      metadata->monotonic_timestamp == metadata->sequence,
                  "both clocks belong to the protected generation under concurrent overwrite");
            std::array<std::byte, test_payload_bytes> copy;
            std::memcpy(copy.data(), stress->payload(*metadata).data(), copy.size());
            stress->unprotect(0);
            for (auto byte : copy) {
                check(byte == static_cast<std::byte>(metadata->sequence % payload_pattern_modulus),
                      "no torn payload under concurrent overwrite");
            }
            cursor = metadata->sequence;
            ++copies;
        }

        writing_thread.join();
        check(!failed.load() && copies > 0, "concurrent reader/writer completed");
        descriptor.control = shared_name('c');
        descriptor.data = shared_name('d');
        descriptor.bytes = 1025;
        // Scenario: odd byte budgets retain alignment; identifiers reject exhaustion.
        auto odd = take(RingBuffer::create(descriptor));
        auto odd_first = take(odd->reserve(closed));
        ok(odd->publish(odd_first, 64, 1, 1, 1));
        shared_store(odd->reader(0).lost_messages, 7);
        const auto observed = odd->candidate(0, true);
        check(observed && odd->protect(*observed, 0), "diagnostic fixture protects the published slot");
        odd->unprotect(0);
        check(shared_load(odd->reader(0).lost_messages) == 7, "copy release preserves cumulative reader diagnostics");
        odd->reset_reader(0);
        check(shared_load(odd->reader(0).lost_messages) == 0, "quiescent index reuse resets diagnostics");
        odd->header().version = 2;
        check(!RingBuffer::open(descriptor, false), "old reader diagnostic layout rejected");
        odd->header().version = ring_buffer_layout_version;
        auto odd_second = take(odd->reserve(closed));
        // Layout boundary: inspect pointer bits only to check the promised 8-byte alignment.
        check(odd_second.bytes.size() == 1025 &&
                  reinterpret_cast<std::uintptr_t>(odd_second.bytes.data()) % 8 == 0,
              "arbitrary byte budget preserves slot alignment without expanding message limit");
        odd->abandon(odd_second);
        shared_store(odd->header().generation, (UINT64_MAX >> 2) - 1);
        auto exhausted = odd->reserve(closed);
        check(!exhausted && exhausted.error().is(ErrorCode::ResourceExhausted),
              "generation never wraps into ABA");
        auto counter_bytes = os_take(counter->mapping.writable_bytes());
        // Fault injection at the shared ABI boundary: this mapping contains one
        // aligned SharedWord whose lifetime began in NamedCounter::create().
        shared_store(*reinterpret_cast<SharedWord*>(counter_bytes.data()), UINT64_MAX);
        auto count_exhausted = counter->next();
        check(!count_exhausted && count_exhausted.error().is(ErrorCode::ResourceExhausted),
              "topic count never wraps");
        std::cout << "message_bus ring buffer tests passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}

#include "ring.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
#include <vinc/osal/process.hpp>
#include <cstring>
#include <array>
#include <thread>
using namespace vinc::bus;
using namespace vinc::bus::detail;
using namespace std::chrono_literals;
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class T> T take(Result<T> r) { if (!r) throw std::runtime_error(r.error().message); return std::move(*r); }
void ok(Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
template<class T> T os_take(vinc::osal::Result<T> r) { if (!r) throw std::runtime_error(r.error().native.message()); return std::move(*r); }
struct Children {
    std::vector<vinc::osal::Process> processes;
    ~Children() { for (auto& p : processes) { (void)p.terminate(); (void)p.wait_until(std::chrono::steady_clock::now() + 3s); } }
};
int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--counter") {
            auto counter = take(NamedCounter::open(argv[2]));
            auto second = os_take(counter->memory.map(sizeof(SharedWord), vinc::osal::SharedMemoryAccess::ReadWrite));
            const auto parent_address = std::stoull(argv[3]);
            if (reinterpret_cast<std::uintptr_t>(counter->mapping.bytes().data()) == parent_address) counter->mapping = std::move(second);
            check(reinterpret_cast<std::uintptr_t>(counter->mapping.bytes().data()) != parent_address, "IPC mapping address differs");
            check(vinc::osal::monotonic_nanoseconds() >= std::stoull(argv[4]), "same-host monotonic clock across processes");
            for (int i = 0; i < 20000; ++i) (void)take(counter->next());
            return 0;
        }
        check(argc == 2, "expected executable path");
        auto counter = take(NamedCounter::create(shared_name('t')));
        auto service = os_take(vinc::osal::ProcessService::create()); Children children;
        for (int i = 0; i < 4; ++i) {
            vinc::osal::SpawnOptions options; options.executable = std::filesystem::absolute(argv[1]);
            options.arguments = {"--counter", counter->name, std::to_string(reinterpret_cast<std::uintptr_t>(counter->mapping.bytes().data())), std::to_string(vinc::osal::monotonic_nanoseconds())};
            children.processes.push_back(os_take(service.spawn(options)));
        }
        for (auto& child : children.processes) {
            const auto exit = os_take(child.wait_until(std::chrono::steady_clock::now() + 15s));
            check(exit.kind == vinc::osal::ExitKind::Code && exit.value == 0, "IPC atomic helper");
        }
        check(take(counter->next()) == 80001, "IPC counter has no lost updates");
        Metadata equal_a{1, 100, 900}, equal_b{999, 100, 1};
        check(preferred(equal_a, 1, equal_b, 2, false) && preferred(equal_a, 1, equal_b, 2, true), "ties use publisher rank, not counter");
        equal_a.timestamp = 99;
        check(preferred(equal_a, 9, equal_b, 1, false) && !preferred(equal_a, 9, equal_b, 1, true), "timestamp is cross-publisher key");
        Descriptor d; d.id = 1; d.owner = 1; d.schema = 42; d.rank = 1; d.slots = 2; d.readers = 4; d.bytes = 1024;
        d.control = shared_name('c'); d.data = shared_name('d');
        auto creator = take(Ring::create(d)); auto writer = take(Ring::open(d, true)); auto reader = take(Ring::open(d, false));
        auto invalid = d; invalid.schema = 43;
        auto rejected = Ring::open(invalid, false);
        check(!rejected && rejected.error().code == ErrorCode::Protocol, "schema mismatch rejected before payload mapping");
        invalid = d; invalid.bytes = UINT64_MAX;
        rejected = Ring::open(invalid, false);
        check(!rejected && rejected.error().code == ErrorCode::Protocol, "descriptor size overflow rejected");
        std::atomic<bool> closed{false};
        auto first = take(writer->reserve(closed)); first.bytes[0] = std::byte{11}; ok(writer->publish(first, 64, 100, 1));
        auto a = reader->candidate(0, false); check(a && a->sequence == 1, "first candidate");
        check(reader->protect(*a, 0), "protect first generation");
        auto second = take(writer->reserve(closed)); second.bytes[0] = std::byte{22}; ok(writer->publish(second, 64, 101, 2));
        auto future = std::async(std::launch::async, [&] { return writer->reserve(closed); });
        struct Unpin { std::shared_ptr<Ring> ring; ~Unpin() { ring->unprotect(0); } } unpin{reader};
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!writer->stats(2).current_wait_ns && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        check(writer->stats(2).collisions == 1, "one collision per wait episode");
        check(future.wait_for(20ms) == std::future_status::timeout, "writer waits while copy protected");
        check(!reader->protect(*a, 1), "closing blocks new readers");
        check(reader->payload(*a)[0] == std::byte{11}, "protected data remains intact");
        reader->unprotect(0);
        auto third = take(future.get());
        check(!reader->protect(*a, 0), "stale generation rejected");
        auto latest = reader->candidate(0, true); check(latest && latest->sequence == 2, "latest survives outstanding loan");
        third.bytes[0] = std::byte{33}; ok(writer->publish(third, 64, 102, 3));
        auto oldest = reader->candidate(0, false); check(oldest && oldest->sequence == 2, "slow reader skips overwritten sequence");
        check(writer->stats(2).total_wait_ns > 0 && writer->stats(2).current_wait_ns == 0, "wait duration completed");
        creator->retire(); check(!reader->candidate(0, true), "retired ring is not readable");
        d.control = shared_name('c'); d.data = shared_name('d');
        auto stress = take(Ring::create(d)); std::atomic<bool> finished{false}, failed{false}, cancel{false};
        std::jthread writing_thread([&] {
            for (std::uint64_t seq = 1; seq <= 3000; ++seq) {
                auto r = stress->reserve(cancel); if (!r) { if (!cancel.load()) failed.store(true); break; }
                std::memset(r->bytes.data(), static_cast<int>(seq % 251), 1024);
                if (!stress->publish(*r, 1024, seq, seq)) { failed.store(true); break; }
            }
            finished.store(true);
        });
        struct Release { std::shared_ptr<Ring> ring; std::atomic<bool>& cancel; ~Release() { ring->unprotect(0); cancel.store(true); } } release{stress, cancel};
        std::uint64_t cursor = 0, copies = 0; const auto stress_end = std::chrono::steady_clock::now() + 10s;
        while (!finished.load() || cursor < shared_load(stress->header().head)) {
            check(std::chrono::steady_clock::now() < stress_end, "bounded stress test");
            auto m = stress->candidate(cursor, false); if (!m || !stress->protect(*m, 0)) { std::this_thread::yield(); continue; }
            std::array<std::byte, 1024> copy; std::memcpy(copy.data(), stress->payload(*m).data(), copy.size()); stress->unprotect(0);
            for (auto b : copy) check(b == static_cast<std::byte>(m->sequence % 251), "no torn payload under concurrent overwrite");
            cursor = m->sequence; ++copies;
        }
        writing_thread.join(); check(!failed.load() && copies > 0, "concurrent reader/writer completed");
        d.control = shared_name('c'); d.data = shared_name('d'); d.bytes = 1025;
        auto odd = take(Ring::create(d));
        auto odd_first = take(odd->reserve(closed)); ok(odd->publish(odd_first, 64, 1, 1));
        auto odd_second = take(odd->reserve(closed));
        check(odd_second.bytes.size() == 1025 && reinterpret_cast<std::uintptr_t>(odd_second.bytes.data()) % 8 == 0,
            "arbitrary byte budget preserves slot alignment without expanding message limit");
        odd->abandon(odd_second);
        shared_store(odd->header().generation, (UINT64_MAX >> 2) - 1);
        auto exhausted = odd->reserve(closed);
        check(!exhausted && exhausted.error().code == ErrorCode::ResourceExhausted, "generation never wraps into ABA");
        auto counter_bytes = os_take(counter->mapping.writable_bytes());
        shared_store(*reinterpret_cast<SharedWord*>(counter_bytes.data()), UINT64_MAX);
        auto count_exhausted = counter->next();
        check(!count_exhausted && count_exhausted.error().code == ErrorCode::ResourceExhausted, "topic count never wraps");
        std::cout << "bus ring tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

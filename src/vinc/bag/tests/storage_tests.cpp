#include <vinc/bag/detail/database.hpp>
#include <vinc/osal/process.hpp>
#include <vinc/osal/path.hpp>
#include <vinc_messages/Frame.hpp>
#include <sqlite3.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

namespace {
using namespace vinc;
using namespace vinc::bag;
using namespace std::chrono_literals;
constexpr auto deadline_budget = 10s;
constexpr auto observe_interval = 10ms;
constexpr std::size_t mib = 1024 * 1024;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }

    return std::move(*result);
}

void take(Result<void> result) {
    if (!result) {
        throw std::runtime_error(error_to_string(result.error()));
    }
}

template <class Predicate> void until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + deadline_budget;
    while (!predicate()) {
        check(std::chrono::steady_clock::now() < deadline, "bounded observation");
        std::this_thread::sleep_for(observe_interval);
    }
}

Result<message::Snapshot> encode(const BaseMessage& value, std::uint64_t limit) {
    return static_cast<const Frame&>(value).snapshot(limit);
}

std::unique_ptr<Frame> frame(std::size_t bytes) {
    auto value = make_message<Frame>();
    take(value->data().payload.resize(bytes));
    if (bytes) {
        value->data().payload[0] = 0x5a;
        value->data().payload[bytes - 1] = 0x5a;
    }

    take(value->data().detections.resize(1));
    value->data().detections[0].data().id = 42;
    take(value->data().detections[0].data().label.assign("nested"));
    return value;
}

void queue(Recorder& recorder, std::size_t bytes) {
    auto value = frame(bytes);
    const auto usage = take(value->usage());
    take(recorder.enqueue(std::move(value), usage.allocated_bytes, encode));
}

void discover(Recorder& recorder) {
    take(recorder.discover({"unknown", "Frame", Frame::schema().hash, true}));
}

void check_payloads(const std::filesystem::path& directory) {
    sqlite3* database = nullptr;
    auto path = take(osal::path_to_utf8(directory / "recording.sqlite"));
    check(sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
          "open recorded payloads");
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(database, "SELECT payload FROM messages ORDER BY id", -1, &statement, nullptr) ==
              SQLITE_OK,
          "prepare payloads");
    std::size_t count = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* bytes = static_cast<const std::byte*>(sqlite3_column_blob(statement, 0));
        const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement, 0));
        auto value = take(Frame::from_bytes({bytes, size}));
        check(value->data().payload.size() == (mib << count), "round-trip payload length");
        check(value->data().payload[0] == 0x5a &&
                  value->data().payload[value->data().payload.size() - 1] == 0x5a,
              "round-trip payload bytes");
        check(value->data().detections[0].data().id == 42, "nested record");
        check(value->data().detections[0].data().label.str() == "nested", "nested text");
        ++count;
    }

    sqlite3_finalize(statement);
    sqlite3_close(database);
    check(count == 8, "all eight payload sizes");
}

std::atomic<bool> encoding{false};
std::atomic<bool> release_encoder{false};

Result<message::Snapshot> stalled_encoder(const BaseMessage& value, std::uint64_t limit) {
    encoding = true;
    const auto deadline = std::chrono::steady_clock::now() + deadline_budget;
    while (!release_encoder && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(observe_interval);
    }

    return encode(value, limit);
}

void test_queue(const std::filesystem::path& path) {
    Options options;
    options.message_bytes = mib;
    options.queue_bytes = mib;
    options.queue_records = 1;
    auto recorder = take(Recorder::create(path, options));
    discover(*recorder);
    auto first = frame(32);
    const auto bytes = take(first->usage()).allocated_bytes;
    take(recorder->enqueue(std::move(first), bytes, stalled_encoder));
    until([] {
        return encoding.load();
    });
    auto rejected = recorder->enqueue(frame(32), bytes, encode);
    check(!rejected && rejected.error().is(ErrorCode::CapacityExceeded),
          "in-flight record retains queue credit");
    const auto state = recorder->status();
    check(state.pending_records == 1 && state.pending_bytes == bytes && state.counters.queue_dropped == 1,
          "bounded pending accounting");
    recorder->finish();
    check(!recorder->status().finished, "finish is asynchronous");
    release_encoder = true;
    take(recorder->wait());
    auto info = take(inspect(path));
    check(info.completed && info.counters.written == 1 && info.counters.queue_dropped == 1,
          "drain and persist counters");
}

void test_database_failure(const std::filesystem::path& path) {
    auto recorder = take(Recorder::create(path));
    discover(*recorder);
    sqlite3* database = nullptr;
    const auto filename = take(osal::path_to_utf8(path / "recording.sqlite"));
    check(sqlite3_open(filename.c_str(), &database) == SQLITE_OK, "open conflicting writer");
    check(sqlite3_exec(database, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr) == SQLITE_OK,
          "hold conflicting transaction");
    queue(*recorder, 32);
    until([&] {
        return recorder->status().finished;
    });
    auto finished = recorder->wait();
    sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(database);
    check(!finished && finished.error().is(ErrorCode::DatabaseFailed),
          "database fault becomes recording failure");
    check(!take(inspect(path)).completed, "failed recording stays incomplete");
    check(!Recorder::create(path / "absent-parent" / "cannot-create"),
          "inaccessible parent reports file failure");
}

// Small fixtures exercise byte limits without allocating a production-size file.
constexpr std::uint64_t test_page_bytes = 4096;
constexpr std::uint64_t test_database_bytes = 4 * mib;
constexpr std::uint64_t sqlite_maximum_bytes = 4294967294ULL * test_page_bytes;

void test_database_limits(const std::filesystem::path& root) {
    const std::uint64_t invalid_limits[] = {0, config::bag_min_database_bytes - 1, sqlite_maximum_bytes + 1,
                                            UINT64_MAX};
    for (const auto bytes : invalid_limits) {
        const auto path = root / ("invalid-limit-" + std::to_string(bytes));
        auto result = Recorder::create(path, {.database_bytes = bytes});
        check(!result && result.error().is(ErrorCode::InvalidArgument), "invalid database limit rejected");
        check(!std::filesystem::exists(path), "invalid limit creates no directory");
    }

    // Setting an upper bound must not preallocate that amount of disk space.
    auto maximum = take(Recorder::create(root / "maximum-limit", {.database_bytes = sqlite_maximum_bytes}));
    take(maximum->wait());
    check(std::filesystem::file_size(root / "maximum-limit" / "recording.sqlite") < mib,
          "maximum accepted without preallocation");

    const auto path = root / "full";
    const auto requested_bytes = test_database_bytes + test_page_bytes - 1;
    auto recorder = take(Recorder::create(path, {.database_bytes = requested_bytes}));
    discover(*recorder);
    queue(*recorder, 2 * mib);
    until([&] {
        return recorder->status().counters.written == 1;
    });
    check(std::filesystem::file_size(path / "recording.sqlite") > test_database_bytes / 4,
          "committed data uses more than the former quarter budget");

    // Hold the oversized transaction in the encoder while another record queues.
    // Failure must discard both, without retrying the smaller queued record.
    encoding = false;
    release_encoder = false;
    auto oversized = frame(test_database_bytes);
    const auto retained = take(oversized->usage()).allocated_bytes;
    take(recorder->enqueue(std::move(oversized), retained, stalled_encoder));
    until([] {
        return encoding.load();
    });
    queue(*recorder, 32);
    recorder->finish();
    release_encoder = true;
    auto result = recorder->wait();
    check(!result && result.error().is(ErrorCode::CapacityExceeded), "database limit stops recording");
    const auto state = recorder->status();
    check(state.finished && !state.accepting && state.counters.written == 1 && state.pending_records == 0 &&
              state.pending_bytes == 0,
          "failed transaction and queued data discarded");
    auto saved = take(inspect(path));
    check(!saved.completed && saved.counters.written == 1 && saved.topics.front().records == 1,
          "previous transaction retained without false completion");
    const auto file_bytes = std::filesystem::file_size(path / "recording.sqlite");
    check(file_bytes <= test_database_bytes && file_bytes % test_page_bytes == 0,
          "non-page-aligned limit rounds down");

    // The earlier append in the SAME transaction must also disappear on FULL.
    const auto batch_path = root / "batch-rollback";
    vinc::bag::detail::Database database;
    take(database.create(batch_path, {.database_bytes = test_database_bytes}));
    take(database.begin());
    take(database.topic({"unknown", "Frame", Frame::schema().hash, true}));
    vinc::bag::detail::Record small{frame(32)};
    auto small_image = take(encode(*small.message, config::bag_message_bytes));
    take(database.append(small, small_image.bytes()));
    take(database.commit({1, 1}, false));
    take(database.begin());
    take(database.append(small, small_image.bytes()));
    vinc::bag::detail::Record large{frame(test_database_bytes)};
    auto large_image = take(encode(*large.message, config::bag_message_bytes));
    auto failed = database.append(large, large_image.bytes());
    check(!failed && failed.error().is(ErrorCode::CapacityExceeded), "later append fills transaction");
    database.rollback();
    take(database.close());
    const auto rolled_back = take(inspect(batch_path));
    check(rolled_back.counters.written == 1 && rolled_back.topics.front().records == 1,
          "whole batch rolled back, including its successful first append");
}

void crash(const std::filesystem::path& directory) {
    vinc::bag::detail::Database database;
    take(database.create(directory, {}));
    take(database.begin());
    take(database.topic({"unknown", "Frame", Frame::schema().hash, true}));
    auto value = frame(32);
    auto image = take(value->snapshot());
    vinc::bag::detail::Record record{std::move(value)};
    take(database.append(record, image.bytes()));
    take(database.commit({1, 1}, false));
    take(database.begin());
    // Spill an uncommitted transaction past SQLite's cache before process death.
    auto large = frame(16 * mib);
    auto large_image = take(large->snapshot());
    vinc::bag::detail::Record uncommitted{std::move(large)};
    take(database.append(uncommitted, large_image.bytes()));
    std::_Exit(42);
}

void recover(const std::filesystem::path& directory) {
    // SQLite needs write access to roll back a hot rollback journal. Normal info
    // inspection is read-only; this explicit test recovery uses SQLite's own path.
    sqlite3* database = nullptr;
    const auto path = take(osal::path_to_utf8(directory / "recording.sqlite"));
    check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "open recovery");
    check(sqlite3_exec(database, "PRAGMA integrity_check", nullptr, nullptr, nullptr) == SQLITE_OK,
          "SQLite crash recovery");
    sqlite3_close(database);
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "crash") {
            crash(take(vinc::osal::path_from_utf8(argv[2])));
        }

        const auto root =
            std::filesystem::temp_directory_path() / ("vinc-bag-" + std::to_string(std::random_device{}()));
        check(std::filesystem::create_directory(root), "exclusive test directory");
        auto recorder = take(vinc::bag::Recorder::create(root / "large"));
        discover(*recorder);
        take(recorder->discover({"foreign/topic", "Foreign", UINT64_MAX, false}));
        for (std::size_t size = mib; size <= 128 * mib; size *= 2) {
            queue(*recorder, size);
            until([&] {
                return recorder->status().pending_records == 0;
            });
        }

        take(recorder->wait());
        auto info = take(vinc::bag::inspect(root / "large"));
        check(info.completed && info.counters.written == 8 && info.topics.size() == 2,
              "known and unknown metadata");
        check_payloads(root / "large");
        check(!vinc::bag::Recorder::create(root / "large"), "never overwrite a recording");
        check(!vinc::bag::inspect(root / "missing"), "missing bag rejected");
        const auto unicode_path = root / std::filesystem::path(u8"錄製🚀");
        auto unicode = take(vinc::bag::Recorder::create(unicode_path));
        take(unicode->wait());
        check(take(vinc::bag::inspect(unicode_path)).completed, "SQLite UTF-8 filename");
        auto aborted = take(vinc::bag::Recorder::create(root / "aborted"));
        discover(*aborted);
        queue(*aborted, 32);
        aborted->finish(vinc::Error{vinc::bag::ErrorCode::BusFailed, vinc::bag::ErrorOperation::Discover});
        check(!aborted->wait(), "external admission failure remains visible");
        auto incomplete = take(vinc::bag::inspect(root / "aborted"));
        check(!incomplete.completed && incomplete.counters.written == 1,
              "drain valid messages without claiming success");
        test_queue(root / "queue");
        test_database_failure(root / "failure");
        test_database_limits(root);
        auto service = take(vinc::osal::ProcessService::create());
        vinc::osal::SpawnOptions options;
        options.executable = std::filesystem::absolute(argv[0]);
        options.arguments = {"crash", take(vinc::osal::path_to_utf8(root / "crash"))};
        auto process = take(service.spawn(options));
        auto exited = process.wait_until(std::chrono::steady_clock::now() + deadline_budget);
        if (!exited) {
            (void)process.terminate();
            (void)process.wait_until(std::chrono::steady_clock::now() + deadline_budget);
        }

        check(exited && exited->value == 42, "crash child terminated");
        recover(root / "crash");
        auto recovered = take(vinc::bag::inspect(root / "crash"));
        check(!recovered.completed && recovered.counters.written == 1 &&
                  recovered.topics.front().records == 1,
              "retain only committed crash data");
        std::cout << "bag storage passed: " << root.string() << '\n';
        recorder.reset();
        check(std::filesystem::weakly_canonical(root).parent_path() ==
                  std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()),
              "cleanup boundary");
        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}

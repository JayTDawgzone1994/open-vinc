// Verify access contracts independently of the layout implementation. Allocation
// interception is confined to this executable and to explicitly watched scopes.
#include <vinc_messages/all_messages.hpp>
#include <vinc/message/detail/framework_access.hpp>
#include "contract_probe.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <type_traits>
#if defined(_WIN32)
#include <malloc.h>
#endif
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace allocation_watch {
thread_local bool active = false;
thread_local bool reject = false;
thread_local std::size_t count = 0;
thread_local std::size_t fail_at = 0;
thread_local bool injected = false;

void allocation() {
    if (active) {
        ++count;
        if (reject && (fail_at == 0 || count == fail_at)) {
            injected = true;
            throw std::bad_alloc();
        }
    }
}

struct Scope {
    explicit Scope(bool fail = false, std::size_t allocation_index = 0) {
        count = 0;
        reject = fail;
        fail_at = allocation_index;
        injected = false;
        active = true;
    }

    ~Scope() {
        active = false;
        reject = false;
    }
};
}

void snapshot_validation_tests();

void* operator new(std::size_t size) {
    allocation_watch::allocation();
    if (auto* value = std::malloc(size == 0 ? 1 : size)) {
        return value;
    }

    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* value) noexcept {
    std::free(value);
}

void operator delete[](void* value) noexcept {
    std::free(value);
}

void operator delete(void* value, std::size_t) noexcept {
    std::free(value);
}

void operator delete[](void* value, std::size_t) noexcept {
    std::free(value);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    allocation_watch::allocation();
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    auto* value = _aligned_malloc(size == 0 ? 1 : size, align);
#else
    const auto rounded = ((size == 0 ? 1 : size) + align - 1) / align * align;
    auto* value = std::aligned_alloc(align, rounded);
#endif
    if (value) {
        return value;
    }

    throw std::bad_alloc();
}

void operator delete(void* value, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(value);
#else
    std::free(value);
#endif
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete[](void* value, std::align_val_t alignment) noexcept {
    ::operator delete(value, alignment);
}

void operator delete(void* value, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(value, alignment);
}

void operator delete[](void* value, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(value, alignment);
}

namespace {
using vinc::message::ErrorCode;
using Framework = vinc::message::detail::FrameworkAccess;

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error("unexpected failed Result");
    }
    return std::move(*result);
}

void check(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}

void ok(vinc::Result<void> result) {
    check(bool(result), "unexpected failed operation");
}

// Each trial starts with fresh state and fails exactly one allocation. Stop only
// when the requested failure index lies beyond the successful operation's path.
template <class Trial> void sweep_allocations(const char* name, Trial trial) {
    std::cerr << "Checking allocation failures: " << name << '\n';
    constexpr std::size_t maximum_failure_index = 2048;
    for (std::size_t failure_index = 1; failure_index <= maximum_failure_index; ++failure_index) {
        if (trial(failure_index)) {
            check(failure_index > 1, "allocation sweep did not exercise any allocation");
            std::cout << "PASS: " << name << ", " << failure_index - 1 << " allocation sites\n";
            return;
        }
    }
    check(false, "allocation sweep exceeded its bounded budget");
}

template <class Operation> auto fail_allocation(std::size_t failure_index, Operation operation) {
    allocation_watch::Scope watched(true, failure_index);
    return operation();
}

template <class T> bool sweep_succeeded(const vinc::Result<T>& result) {
    if (!allocation_watch::injected) {
        check(bool(result), "operation failed without allocation injection");
        return true;
    }

    check(!result && result.error().is(ErrorCode::ResourceExhausted),
          "injected allocation did not return ResourceExhausted");
    return false;
}

std::vector<std::byte> saved_bytes(const vinc::message::Snapshot& snapshot) {
    return {snapshot.bytes().begin(), snapshot.bytes().end()};
}

void allocation_failure_tests() {
    sweep_allocations("typed snapshot validation", [](std::size_t failure_index) {
        vinc::MessageArray<vinc::Detection> records;
        ok(records.resize(1));
        records[0].data().id = 41;
        auto snapshot = take(records.snapshot());
        const auto before = saved_bytes(snapshot);
        auto previous = take(snapshot.array<vinc::Detection>(0));
        auto result = fail_allocation(failure_index, [&] {
            return snapshot.array<vinc::Detection>(0);
        });
        const bool finished = sweep_succeeded(result);
        check(saved_bytes(snapshot) == before && previous[0].data().id.get() == 41,
              "schema validation changed the snapshot or existing view");
        check(take(snapshot.array<vinc::Detection>(0))[0].data().id.get() == 41,
              "schema validation retry failed");
        return finished;
    });

#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
    // MSVC's noexcept vector constructors allocate debug iterator proxies.
    // Failing those allocations terminates inside the STL before our Result
    // boundary can catch them. Sweep the remaining paths in Release, without
    // disabling iterator checks or changing the component's exception policy.
    std::cout << "SKIP: remaining allocation sweeps require MSVC iterator debugging disabled (Release)\n";
#else
    constexpr std::size_t loan_capacity_bytes = 16 * 1024;
    constexpr std::size_t expanded_record_count = 3;

    sweep_allocations("record at copy and wrappers", [](std::size_t failure_index) {
        vinc::MessageArray<vinc::Detection> records;
        ok(records.resize(1));
        records[0].data().id = 42;
        ok(records[0].data().extra.resize(5));
        auto previous = records[0];
        const auto before = saved_bytes(take(records.snapshot()));
        auto result = fail_allocation(failure_index, [&] {
            return records.at(0);
        });
        const bool finished = sweep_succeeded(result);
        check(saved_bytes(take(records.snapshot())) == before && previous.data().id.get() == 42,
              "record copy changed its source or invalidated its proxy");
        auto retry = take(records.at(0));
        retry.data().id = 43;
        check(previous.data().id.get() == 42, "retried record copy still aliases source");
        return finished;
    });

    sweep_allocations("fixed loan resize rollback", [&](std::size_t failure_index) {
        auto backing = std::make_shared<std::vector<std::byte>>(loan_capacity_bytes);
        auto loan = take(Framework::borrow_message<vinc::Frame>(*backing, backing, loan_capacity_bytes));
        ok(loan->data().detections.resize(1));
        loan->data().detections[0].data().id = 44;
        auto previous = loan->data().detections[0];
        const auto before = *backing;
        auto result = fail_allocation(failure_index, [&] {
            return loan->data().detections.resize(expanded_record_count);
        });
        if (sweep_succeeded(result)) {
            check(loan->data().detections.size() == expanded_record_count, "resize success lost size");
            return true;
        }

        check(*backing == before && loan->data().detections.size() == 1 && previous.data().id.get() == 44,
              "failed resize changed bytes, size or proxy validity");
        ok(loan->data().detections.resize(expanded_record_count));
        check(loan->data().detections[0].data().id.get() == 44, "resize retry lost original element");
        return false;
    });

    vinc::Frame source;
    source.data().sequence = 45;
    ok(source.data().detections.resize(2));
    source.data().detections[1].data().id = 46;
    std::vector<std::uint8_t> encoded;
    ok(source.serialize(encoded));

    sweep_allocations("fixed loan deserialize rollback", [&](std::size_t failure_index) {
        auto backing = std::make_shared<std::vector<std::byte>>(loan_capacity_bytes);
        auto loan = take(Framework::borrow_message<vinc::Frame>(*backing, backing, loan_capacity_bytes));
        loan->data().sequence = 47;
        ok(loan->data().detections.resize(1));
        loan->data().detections[0].data().id = 48;
        auto previous = loan->data().detections[0];
        const auto before = *backing;
        std::size_t offset = 0;
        auto result = fail_allocation(failure_index, [&] {
            return loan->deserialize(encoded, offset);
        });
        if (sweep_succeeded(result)) {
            check(offset == encoded.size() && loan->data().detections[1].data().id.get() == 46,
                  "deserialize success lost cursor or payload");
            return true;
        }

        check(*backing == before && offset == 0 && loan->data().sequence.get() == 47 &&
                  previous.data().id.get() == 48,
              "failed deserialize changed loan, cursor or existing proxy");
        ok(loan->deserialize(encoded, offset));
        check(offset == encoded.size() && loan->data().sequence.get() == 45,
              "deserialize retry failed");
        return false;
    });

    sweep_allocations("seal retains loan on failure", [&](std::size_t failure_index) {
        auto backing = std::make_shared<std::vector<std::byte>>(loan_capacity_bytes);
        auto loan = take(Framework::borrow_message<vinc::Frame>(*backing, backing, loan_capacity_bytes));
        ok(loan->data().detections.resize(1));
        loan->data().detections[0].data().id = 49;
        auto previous = loan->data().detections[0];
        const auto before = *backing;
        auto result = fail_allocation(failure_index, [&] {
            return Framework::seal(*loan);
        });
        if (sweep_succeeded(result)) {
            check(result->bytes().data() == backing->data(), "seal copied the loan payload");
            return true;
        }

        check(*backing == before && previous.data().id.get() == 49,
              "failed seal changed bytes or revoked the loan");
        auto retry = take(Framework::seal(*loan));
        check(retry.bytes().data() == backing->data(), "seal retry copied the payload");
        return false;
    });

    sweep_allocations("push preserves destination on failure", [&](std::size_t failure_index) {
        auto backing = std::make_shared<std::vector<std::byte>>(loan_capacity_bytes, std::byte{0x5a});
        const auto before = *backing;
        const auto source_before = saved_bytes(take(source.snapshot()));
        auto previous = source.data().detections[1];
        auto result = fail_allocation(failure_index, [&] {
            return Framework::push(source, *backing, backing);
        });
        const bool finished = sweep_succeeded(result);
        check(saved_bytes(take(source.snapshot())) == source_before && previous.data().id.get() == 46,
              "push changed the source or invalidated its proxy");
        if (finished) {
            check(result->bytes().data() == backing->data(), "push returned a different destination");
            return true;
        }

        check(*backing == before && backing.use_count() == 1,
              "failed push changed destination or retained its owner");
        auto retry = take(Framework::push(source, *backing, backing));
        check(retry.bytes().data() == backing->data(), "push retry used the wrong destination");
        return false;
    });

    sweep_allocations("borrow releases owner on failure", [&](std::size_t failure_index) {
        auto backing = std::make_shared<std::vector<std::byte>>(loan_capacity_bytes);
        auto result = fail_allocation(failure_index, [&] {
            return Framework::borrow_message<vinc::Frame>(*backing, backing, loan_capacity_bytes);
        });
        if (sweep_succeeded(result)) {
            (*result)->data().sequence = 50;
            return true;
        }

        // Initialization may have written bytes before a facade allocation fails;
        // no loan was delivered, so the required recovery is release and reuse.
        check(backing.use_count() == 1, "failed borrow retained the external owner");
        auto retry = take(Framework::borrow_message<vinc::Frame>(*backing, backing, loan_capacity_bytes));
        retry->data().sequence = 51;
        check(retry->data().sequence.get() == 51, "borrow retry did not restore usable storage");
        Framework::revoke(*retry);
        check(backing.use_count() == 1, "revoke retained the external owner");
        return false;
    });
#endif
}

using ReadRecord = vinc::message::RecordRef<vinc::Detection, true>;
using WriteRecord = vinc::message::RecordRef<vinc::Detection, false>;
using ReadScalar = decltype(std::declval<ReadRecord&>().data().id);
static_assert(!std::is_assignable_v<ReadScalar&, std::uint32_t>);
static_assert(!std::is_copy_assignable_v<vinc::MessageArray<std::uint8_t>>);
static_assert(!std::is_move_assignable_v<vinc::MessageArray<std::uint8_t>>);
static_assert(!std::is_copy_assignable_v<vinc::MessageArray<std::string>>);
static_assert(!std::is_move_assignable_v<vinc::message::ArrayRef<std::uint8_t, false>>);
static_assert(!std::is_copy_assignable_v<WriteRecord>);
static_assert(!std::is_copy_assignable_v<vinc::Frame>);
static_assert(!std::is_assignable_v<vinc::message::MessageString&, std::string_view>);
static_assert(std::same_as<decltype(std::declval<vinc::MessageArray<std::string>&>().at(0)),
                           vinc::Result<std::string>>);

void array_access_tests() {
    vinc::MessageArray<vinc::Detection> records;
    ok(records.resize(2));
    auto snapshot = take(records.snapshot());
    auto readonly = take(snapshot.array<vinc::Detection>(0));
    bool values_ok = false;
    {
        allocation_watch::Scope watched;
        // First access to each record, its scalar and nested record fields must
        // not create a cache entry, initialize schemas or materialize a message.
        auto first = records[0];
        auto alias = first;
        alias.data().id = 42;
        first.data().position.data().x = 1.5f;
        auto fields = records[1].data();
        fields.id = 9;
        auto frozen = readonly[0];
        values_ok = records[0].data().id.get() == 42 && frozen.data().id.get() == 0 &&
                    first.data().position.data().x.get() == 1.5f;
        for (auto element : records) {
            (void)element.data().id.get();
        }
        for (auto element : readonly) {
            (void)element.data().id.get();
        }
    }
    check(allocation_watch::count == 0 && values_ok, "record access allocated or lost alias semantics");

    vinc::MessageArray<vinc::MessageArray<vinc::Detection>> nested_records;
    ok(nested_records.resize(1));
    ok(nested_records[0].resize(1));
    auto nested_record_snapshot = take(nested_records.snapshot());
    auto nested_record_view = take(nested_record_snapshot.array<vinc::MessageArray<vinc::Detection>>(0));
    {
        allocation_watch::Scope watched;
        // Even the first nested record facade must remain a lightweight borrow.
        auto row = nested_record_view[0];
        auto record = row[0];
        values_ok = record.data().id.get() == 0 && record.data().position.data().x.get() == 0;
    }
    check(allocation_watch::count == 0 && values_ok, "nested record view access allocated");
    auto detached = take(records.at(0));
    detached.data().id = 99;
    check(records[0].data().id.get() == 42, "at() did not detach record value");
    auto frozen_copy = take(readonly.at(0));
    frozen_copy.data().id = 17;
    check(readonly[0].data().id.get() == 0, "view at() did not detach");
    ok(records.push_back(readonly[0]));
    check(records[2].data().id.get() == 0, "snapshot proxy copy source");
    ok(records.set(1, readonly[0]));
    ok(records[1].assign(records[0]));
    check(records[1].data().id.get() == 42, "checked record proxy replacement");
    auto out_of_range = records.at(records.size());
    check(!out_of_range && out_of_range.error().is(ErrorCode::OutOfRange), "checked record bounds");
    auto failed_set = records.set(records.size(), detached);
    check(!failed_set && failed_set.error().is(ErrorCode::OutOfRange), "checked record write bounds");
    contract_test::expect([&] {
        (void)records[records.size()];
    });
    contract_test::expect([&] {
        (void)readonly[readonly.size()];
    });

    vinc::MessageArray<vinc::MessageArray<std::uint32_t>> nested;
    ok(nested.resize(1));
    ok(nested[0].resize(2));
    auto nested_snapshot = take(nested.snapshot());
    auto nested_view = take(nested_snapshot.array<vinc::MessageArray<std::uint32_t>>(0));
    {
        allocation_watch::Scope watched;
        auto row = nested[0];
        auto alias = row;
        alias[0] = 123;
        auto const_row = std::as_const(nested)[0];
        values_ok = const_row[0] == 123 && nested_view[0][0] == 0;
    }
    check(allocation_watch::count == 0 && values_ok, "nested access allocated or copied");
    auto detached_row = take(nested_view.at(0));
    ok(detached_row.set(0, 19));
    check(nested_view[0][0] == 0, "nested view at copy");
    ok(detached_row.assign(nested_view[0]));
    check(detached_row[0] == 0, "assign immutable array proxy");
    ok(nested[0].emplace_back(std::uint32_t{5}));
    check(nested[0][2] == 5, "nested proxy emplace");

    vinc::Frame frame;
    ok(frame.data().labels.resize(1));
    ok(frame.data().labels[0].assign("abc"));
    {
        allocation_watch::Scope watched;
        auto text = frame.data().labels[0];
        text[0] = 'x';
        values_ok = text.size() == 3 && std::as_const(frame).data().labels[0][0] == 'x';
    }
    check(allocation_watch::count == 0 && values_ok, "string proxy allocated during access");
    auto text_copy = take(frame.data().labels[0].copy());
    ok(text_copy.assign("replacement"));
    ok(frame.data().labels[0].assign(text_copy));
    auto copied_text = take(frame.data().labels.at(0));
    check(copied_text.str() == "replacement", "string proxy checked replacement");

    // A sealed image may retain reserved capacity; readonly queries must report
    // the encoded capacity rather than equating it with the current length.
    constexpr std::size_t loan_bytes = 4096;
    constexpr std::uint64_t reserved_elements = 16;
    auto backing = std::make_shared<std::vector<std::byte>>(loan_bytes);
    auto loan = take(Framework::borrow(*backing, backing, loan_bytes));
    auto bytes = take(loan.add_array<std::uint8_t>());
    ok(bytes.reserve(reserved_elements));
    ok(bytes.resize(1));
    auto sealed = take(Framework::seal(loan));
    auto sealed_view = take(sealed.array<std::uint8_t>(0));
    check(sealed_view.size() == 1 && sealed_view.capacity() >= reserved_elements,
          "sealed readonly capacity lost reserved space");

    // Known allocation boundaries still convert standard failures to Result.
    std::vector<std::uint8_t> encoded;
    ok(frame.serialize(encoded));
    vinc::Frame decoded;
    decoded.data().sequence = 73;
    std::size_t offset = 0;
    {
        allocation_watch::Scope watched(true);
        auto result = decoded.deserialize(encoded, offset);
        check(!result && result.error().is(ErrorCode::ResourceExhausted) &&
                  result.error().is_operation(vinc::message::ErrorOperation::Copy),
              "decode allocation failure lost its original error");
    }
    check(offset == 0 && decoded.data().sequence.get() == 73,
          "decode allocation failure changed destination or cursor");
    {
        allocation_watch::Scope watched(true);
        auto result = records.at(0);
        check(!result && result.error().is(ErrorCode::ResourceExhausted),
              "at allocation failure not returned");
    }
    bool native_exception = false;
    try {
        allocation_watch::Scope watched(true);
        auto natural_copy = records;
        (void)natural_copy;
    } catch (const std::bad_alloc&) {
        native_exception = true;
    }
    check(native_exception, "natural copy changed standard allocation exception into a contract");

    vinc::MessageArray<std::string> local;
    ok(local.push_back("local"));
    auto local_copy = take(local.at(0));
    local_copy = "copy";
    check(local[0] == "local", "ordinary at must copy");
    auto missing = local.at(1);
    check(!missing && missing.error().is(ErrorCode::OutOfRange), "ordinary at bounds");
    ok(local.set(0, std::string("updated")));
    vinc::MessageArray<std::unique_ptr<int>> move_source;
    ok(move_source.push_back(std::make_unique<int>(7)));
    vinc::MessageArray<std::unique_ptr<int>> move_target;
    ok(move_target.assign(std::move(move_source)));
    check(*move_target[0] == 7, "named move assignment of local objects");
    contract_test::expect([&] {
        (void)local[1];
    });
}
}

int main(int argc, char** argv) {
#if defined(_MSC_VER)
    // Report CRT assertions to the test runner instead of opening a modal dialog.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    contract_test::initialize(argc, argv);
    try {
        array_access_tests();
        std::cerr << "Checking snapshot schemas\n";
        snapshot_validation_tests();
        std::cerr << "Checking allocation rollback\n";
        allocation_failure_tests();
        std::cout << "PASS: allocation-free proxies, checked access, const, copies and contracts\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

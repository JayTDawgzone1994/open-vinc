// Shared ownership/publication protocol. OSAL supplies native IPC atomics and
// mappings; the supervisor supplies process-exit evidence, never this component.
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <vinc/concurrency/detail/error.hpp>
#include <vinc/concurrency/detail/interprocess_mutex_state.hpp>
#include <vinc/concurrency/detail/mutex_recovery.hpp>
#include <vinc/config.hpp>
#include <vinc/contract.hpp>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <new>
#include <utility>

namespace vinc::concurrency {
Error detail::translate_shared_memory_error(const Error& failure, ErrorOperation operation) noexcept {
    if (failure.domain != ErrorDomain::Osal) {
        return wrap_error(ErrorCode::SystemError, operation, failure);
    }

    switch (static_cast<osal::ErrorCode>(failure.code)) {
    case osal::ErrorCode::InvalidArgument:
        return wrap_error(ErrorCode::InvalidArgument, operation, failure);
    case osal::ErrorCode::NotFound:
        return wrap_error(ErrorCode::NotFound, operation, failure);
    case osal::ErrorCode::PermissionDenied:
        return wrap_error(ErrorCode::PermissionDenied, operation, failure);
    case osal::ErrorCode::ResourceExhausted:
        return wrap_error(ErrorCode::ResourceExhausted, operation, failure);
    case osal::ErrorCode::Unsupported:
        return wrap_error(ErrorCode::Unsupported, operation, failure);
    case osal::ErrorCode::Timeout:
        return wrap_error(ErrorCode::Timeout, operation, failure);
    case osal::ErrorCode::Cancelled:
        return wrap_error(ErrorCode::Cancelled, operation, failure);
    case osal::ErrorCode::Busy:
        return wrap_error(ErrorCode::Busy, operation, failure);
    case osal::ErrorCode::Closed:
        return wrap_error(ErrorCode::Closed, operation, failure);
    case osal::ErrorCode::WouldBlock:
        return wrap_error(ErrorCode::WouldBlock, operation, failure);
    case osal::ErrorCode::AlreadyExists:
        return wrap_error(ErrorCode::AlreadyExists, operation, failure);
    default:
        return wrap_error(ErrorCode::SystemError, operation, failure);
    }
}

namespace {
using State = detail::InterprocessMutexState;
using Header = detail::MutexHeader;
using Clock = std::chrono::steady_clock;

Error error(ErrorCode code, ErrorOperation operation = ErrorOperation::Lock) noexcept {
    return make_error(code, operation);
}

const void* thread_token() noexcept {
    // Local TLS identity is never placed in shared memory or dereferenced.
    thread_local unsigned char token = 0;
    return &token;
}

std::uint64_t encode(OwnerId owner, bool publication) noexcept {
    return (owner << 1) | static_cast<std::uint64_t>(publication);
}

OwnerId decode_owner(std::uint64_t word) noexcept {
    return word >> 1;
}

bool decode_publication(std::uint64_t word) noexcept {
    return (word & detail::publication_mask) != 0;
}

// A newly created name has not been handed to any peer. Best-effort unlink on
// failure/unwinding avoids orphaning POSIX names; Windows has handle lifetime.
struct PendingName {
    State& state;
    bool complete = false;
    ~PendingName() {
        if (!complete) {
            (void)osal::SharedMemory::unlink_name(state.name);
        }
    }
};

Result<void> create_region(State& state) {
    auto memory = osal::SharedMemory::create(state.name, sizeof(Header));
    if (!memory) {
        return std::unexpected(detail::translate_shared_memory_error(memory.error(), ErrorOperation::Create));
    }

    state.memory = std::move(*memory);
    return {};
}

Result<void> open_region(State& state) {
    auto memory = osal::SharedMemory::open(state.name, osal::SharedMemoryAccess::ReadWrite);
    if (!memory) {
        return std::unexpected(detail::translate_shared_memory_error(memory.error(), ErrorOperation::Open));
    }

    state.memory = std::move(*memory);
    return {};
}

Result<void> map_region(State& state, ErrorOperation operation) {
    auto mapping = state.memory.map(sizeof(Header), osal::SharedMemoryAccess::ReadWrite);
    if (!mapping) {
        return std::unexpected(detail::translate_shared_memory_error(mapping.error(), operation));
    }

    state.mapping = std::move(*mapping);
    return {};
}

Result<void> attach_header(State& state, bool initialize) {
    const auto operation = initialize ? ErrorOperation::Create : ErrorOperation::Open;
    auto bytes = state.mapping.writable_bytes();
    if (!bytes) {
        return std::unexpected(detail::translate_shared_memory_error(bytes.error(), operation));
    }

    // Shared-memory ABI boundary: OS mappings start at page alignment, cover
    // sizeof(Header), and all peers use this same native 24-byte layout. Only the
    // creator begins object lifetime, before publishing the resource name.
    auto* header = reinterpret_cast<Header*>(bytes->data());
    if (initialize) {
        std::construct_at(header);
    } else if (header->magic != detail::mutex_magic ||
               header->version != detail::mutex_layout_version || header->bytes != sizeof(Header)) {
        return std::unexpected(error(ErrorCode::InvalidLayout, operation));
    }

    state.header = header;
    return {};
}

Result<std::shared_ptr<State>> make_state(std::string_view name, OwnerId owner) {
    if (!valid_owner(owner)) {
        return std::unexpected(error(ErrorCode::InvalidArgument));
    }

    // Allocate local ownership before acquiring native resources.
    auto state = std::make_shared<State>();
    state->name = name;
    state->identity = owner;
    return state;
}

Result<std::uint64_t> acquire(State& state, OwnerId expected_owner, ErrorOperation operation) noexcept {
    const void* empty = nullptr;
    const auto current_thread = thread_token();
    if (!state.local_owner.compare_exchange_strong(empty, current_thread, std::memory_order_acquire)) {
        const auto code = empty == current_thread ? ErrorCode::Busy : ErrorCode::WouldBlock;
        return std::unexpected(error(code, operation));
    }

    auto observed = osal::shared_load(state.header->control);
    if (decode_owner(observed) != expected_owner) {
        state.local_owner.store(nullptr, std::memory_order_release);
        return std::unexpected(error(ErrorCode::WouldBlock, operation));
    }

    const auto held = encode(state.identity, decode_publication(observed));
    if (!osal::shared_compare_exchange(state.header->control, observed, held)) {
        state.local_owner.store(nullptr, std::memory_order_release);
        return std::unexpected(error(ErrorCode::WouldBlock, operation));
    }

    return held;
}
}

InterprocessMutex::InterprocessMutex(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {
}

InterprocessMutex::operator bool() const noexcept {
    return bool(state_);
}

Result<InterprocessMutex> InterprocessMutex::create(std::string_view name, OwnerId owner) {
    auto prepared = make_state(name, owner);
    if (!prepared) {
        return std::unexpected(error(ErrorCode::InvalidArgument, ErrorOperation::Create));
    }

    auto state = std::move(*prepared);
    auto created = create_region(*state);
    if (!created) {
        return std::unexpected(created.error());
    }

    PendingName pending{*state};
    auto mapped = map_region(*state, ErrorOperation::Create);
    if (!mapped) {
        return std::unexpected(mapped.error());
    }

    auto initialized = attach_header(*state, true);
    if (!initialized) {
        return std::unexpected(initialized.error());
    }

    pending.complete = true;
    return InterprocessMutex(std::move(state));
}

Result<InterprocessMutex> InterprocessMutex::open(std::string_view name, OwnerId owner) {
    auto prepared = make_state(name, owner);
    if (!prepared) {
        return std::unexpected(error(ErrorCode::InvalidArgument, ErrorOperation::Open));
    }

    auto state = std::move(*prepared);
    auto opened = open_region(*state);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    auto mapped = map_region(*state, ErrorOperation::Open);
    if (!mapped) {
        return std::unexpected(mapped.error());
    }

    auto attached = attach_header(*state, false);
    if (!attached) {
        return std::unexpected(attached.error());
    }

    return InterprocessMutex(std::move(state));
}

Result<InterprocessMutexGuard> InterprocessMutex::try_lock() const {
    auto state = state_;
    if (!state) {
        return std::unexpected(error(ErrorCode::Closed));
    }

    auto held = acquire(*state, no_owner, ErrorOperation::Lock);
    if (!held) {
        return std::unexpected(held.error());
    }

    return InterprocessMutexGuard(std::move(state), *held);
}

Result<InterprocessMutexGuard> InterprocessMutex::lock_until(Deadline deadline, std::stop_token stop) const {
    // Retain the mapping throughout retries; no shared/local reservation is
    // held while asleep. The sleep mutex belongs exclusively to this call.
    InterprocessMutex retained(state_);
    if (!retained) {
        return std::unexpected(error(ErrorCode::Closed));
    }

    std::mutex sleep_mutex;
    std::condition_variable_any sleep_changed;
    std::unique_lock sleep_lock(sleep_mutex);
    for (;;) {
        if (stop.stop_requested()) {
            return std::unexpected(error(ErrorCode::Cancelled));
        }

        const auto now = Clock::now();
        if (now >= deadline) {
            return std::unexpected(error(ErrorCode::Timeout));
        }

        auto acquired = retained.try_lock();
        if (acquired || !acquired.error().is(ErrorCode::WouldBlock)) {
            return acquired;
        }

        const auto retry_at = (std::min)(deadline, Clock::now() + config::concurrency_retry_interval);
        sleep_changed.wait_until(sleep_lock, stop, retry_at, [] { return false; });
    }
}

Result<OwnerId> InterprocessMutex::owner() const noexcept {
    if (!state_) {
        return std::unexpected(error(ErrorCode::Closed, ErrorOperation::InspectOwner));
    }

    return decode_owner(osal::shared_load(state_->header->control));
}

void InterprocessMutex::close() noexcept {
    state_.reset();
}

InterprocessMutexGuard::InterprocessMutexGuard(std::shared_ptr<State> state, std::uint64_t held_word,
                                             bool recovered) noexcept
    : state_(std::move(state)), held_word_(held_word), recovered_(recovered) {
}

InterprocessMutexGuard::~InterprocessMutexGuard() {
    release_or_violate();
}

InterprocessMutexGuard::InterprocessMutexGuard(InterprocessMutexGuard&& other) noexcept
    : state_(std::move(other.state_)), held_word_(other.held_word_), recovered_(other.recovered_) {
}

InterprocessMutexGuard& InterprocessMutexGuard::operator=(InterprocessMutexGuard&& other) noexcept {
    if (this != &other) {
        release_or_violate();
        state_ = std::move(other.state_);
        held_word_ = other.held_word_;
        recovered_ = other.recovered_;
    }

    return *this;
}

InterprocessMutexGuard::operator bool() const noexcept {
    return bool(state_);
}

bool InterprocessMutexGuard::publication() const noexcept {
    if (!state_) {
        contract_violation("publication requires a held shared mutex guard");
    }

    return decode_publication(held_word_);
}

bool InterprocessMutexGuard::recovered() const noexcept {
    return bool(state_) && recovered_;
}

Result<void> InterprocessMutexGuard::release(bool publish) noexcept {
    if (!state_) {
        return {};
    }

    if (state_->local_owner.load(std::memory_order_acquire) != thread_token()) {
        const auto operation = publish ? ErrorOperation::Publish : ErrorOperation::Unlock;
        return std::unexpected(error(ErrorCode::InvalidArgument, operation));
    }

    auto expected = held_word_;
    auto selected = decode_publication(held_word_);
    if (publish) {
        selected = !selected;
    }

    // One SC CAS publishes completed writes AND releases ownership. An aborted
    // candidate stays unreachable. No native unlock or allocation follows.
    if (!osal::shared_compare_exchange(state_->header->control, expected, encode(no_owner, selected))) {
        // A live guard cannot lose ownership under the cooperative protocol.
        // This is corruption/invalid recovery, not an ordinary OS resource error.
        contract_violation("shared mutex ownership changed while its guard was held");
    }

    state_->local_owner.store(nullptr, std::memory_order_release);
    state_.reset();
    return {};
}

Result<void> InterprocessMutexGuard::unlock() noexcept {
    return release(false);
}

Result<void> InterprocessMutexGuard::publish_and_unlock() noexcept {
    return release(true);
}

void InterprocessMutexGuard::release_or_violate() noexcept {
    if (!unlock()) {
        contract_violation("shared mutex guard destroyed on a different thread");
    }
}

Result<InterprocessMutexGuard> detail::MutexRecovery::acquire_after_exit(
    const InterprocessMutex& mutex, OwnerId dead_owner) {
    auto state = mutex.state_;
    if (!state) {
        return std::unexpected(error(ErrorCode::Closed, ErrorOperation::Recover));
    }

    if (state->identity != central_owner || dead_owner == central_owner ||
        !valid_owner(dead_owner)) {
        return std::unexpected(error(ErrorCode::InvalidArgument, ErrorOperation::Recover));
    }

    auto held = acquire(*state, dead_owner, ErrorOperation::Recover);
    if (!held) {
        return std::unexpected(held.error());
    }

    return InterprocessMutexGuard(std::move(state), *held, true);
}
}

#pragma once

// Typed cross-process publication for Ribosome-generated messages.
// Socket handles registration/topology; each Publisher owns a shared-memory ring buffer for payloads.

#include <vinc/message/generated.hpp>
#include <vinc/config.hpp>
#include <vinc/message_bus/error.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <string>

namespace vinc::message_bus {

// max_message_bytes is the complete encoded size of one message, including layout
// metadata/alignment. Each Publisher gets its own ring buffer; a requested one slot becomes two.
struct PublisherOptions {
    std::uint32_t slots = config::message_bus_default_slots;
    std::uint64_t max_message_bytes = config::message_bus_default_message_bytes;
};
// OldestRetained permits existing data; NewOnly starts each attached source cursor
// at its current head. get_last() is independent of this callback cursor.
enum class StartPosition {
    OldestRetained,
    NewOnly
};

struct SubscriberOptions {
    StartPosition start = StartPosition::OldestRetained;
    // Zero preserves unrestricted reception. Oversized callback records are skipped
    // before allocation and counted; get_last reports CapacityExceeded instead.
    std::uint64_t max_message_bytes = 0;
};

struct TopicInfo {
    std::string name;
    std::uint64_t schema = 0;
    std::uint32_t publishers = 0;
};

// Pages are individually consistent, not a transaction across changing topology.
struct TopicPage {
    std::vector<TopicInfo> topics;
    std::string next;
};

struct Limits {
    std::uint32_t max_topics = config::message_bus_default_topics;
    std::uint32_t publishers_per_topic = config::message_bus_default_publishers_per_topic;
    std::uint32_t subscribers_per_topic = config::message_bus_default_subscribers_per_topic;
    std::uint64_t max_shared_bytes = config::message_bus_default_shared_bytes;
};

// Observe capacity and overwrite contention. A collision counts one reserve operation
// that had to wait; wait times describe reader protection, not application processing time.
struct PublisherStats {
    std::uint32_t requested_slots = 0;
    std::uint32_t effective_slots = 0;
    std::uint64_t max_message_bytes = 0;
    std::uint64_t shared_bytes = 0;
    std::uint64_t published = 0;
    std::uint64_t collisions = 0;
    std::uint64_t total_wait_ns = 0;
    std::uint64_t longest_wait_ns = 0;
    std::uint64_t current_wait_ns = 0;
};

struct PublisherLoss {
    std::uint64_t publisher = 0;
    std::uint64_t overwrite_events = 0;
    std::uint64_t lost_messages = 0;
};

// Totals retain losses from retired sources without retaining unbounded source history.
struct SubscriberStats {
    std::uint64_t oversized_messages = 0;
    std::uint64_t overwrite_events = 0;
    std::uint64_t lost_messages = 0;
    std::vector<PublisherLoss> sources; // Only currently attached sources; totals include retired ones.
};

namespace detail {
struct PublisherCore;
struct SubscriberCore;
struct LoanCore;
class RuntimeAccess;
class DispatchAccess;

struct LoanBinding {
    std::shared_ptr<LoanCore> loan;
    message::detail::Binding binding;
};

vinc::Result<LoanBinding> borrow(const std::shared_ptr<PublisherCore>&, const message::detail::Schema&);
void construction_failed(const std::shared_ptr<PublisherCore>&, const std::shared_ptr<LoanCore>&) noexcept;
void abandon(const std::shared_ptr<LoanCore>&) noexcept;
bool usable(const std::shared_ptr<LoanCore>&) noexcept;
vinc::Result<void> commit(const std::shared_ptr<PublisherCore>&, const std::shared_ptr<LoanCore>&,
                    message::detail::Binding);
vinc::Result<void> push(const std::shared_ptr<PublisherCore>&, message::detail::Binding);
void close(const std::shared_ptr<PublisherCore>&) noexcept;
void close(const std::shared_ptr<SubscriberCore>&) noexcept;
PublisherStats stats(const std::shared_ptr<PublisherCore>&);
SubscriberStats stats(const std::shared_ptr<SubscriberCore>&);
using Factory = std::unique_ptr<BaseMessage> (*)(message::detail::Binding);
vinc::Result<std::unique_ptr<BaseMessage>> read(const std::shared_ptr<SubscriberCore>&, bool latest,
                                          const message::detail::Schema&, Factory);
}
template <class T>
concept Message = std::is_base_of_v<message::GeneratedMessage<T>, T>;
template <Message T> class Publisher;
template <Message T> class Subscriber;

// Move-only exclusive write access obtained from one Publisher. commit() consumes it.
// Dropping an uncommitted loan intentionally leaves that Publisher blocked from borrowing again.
template <Message T> class WriteLoan {
    friend class Publisher<T>;
    std::shared_ptr<detail::LoanCore> loan_;
    std::unique_ptr<T> value_;

    WriteLoan(detail::LoanBinding binding)
        : loan_(std::move(binding.loan)),
          value_(new T(message::detail::RecordAccess::bind<T>(std::move(binding.binding)))) {
    }

public:
    WriteLoan(const WriteLoan&) = delete;
    WriteLoan& operator=(const WriteLoan&) = delete;
    WriteLoan(WriteLoan&&) noexcept = default;

    WriteLoan& operator=(WriteLoan&& other) noexcept {
        if (this != &other) {
            detail::abandon(loan_);
            value_.reset();
            loan_ = std::move(other.loan_);
            value_ = std::move(other.value_);
        }

        return *this;
    }

    ~WriteLoan() {
        detail::abandon(loan_);
    }

    T* operator->() {
        if (!value_ || !detail::usable(loan_)) {
            vinc::contract_violation("expired Message Bus write loan");
        }

        return value_.get();
    }

    T& operator*() {
        return *operator->();
    }
};

// Host-attached client used by Context. Application modules use their inherited Context.
// Registration may wait for the central BusManager; normal payload transfers use shared memory.
class Client {
    friend class detail::RuntimeAccess;
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit Client(std::shared_ptr<Impl>);
    vinc::Result<std::shared_ptr<detail::PublisherCore>> advertise_raw(std::string, std::uint64_t,
                                                                 PublisherOptions);
    vinc::Result<std::shared_ptr<detail::SubscriberCore>> subscribe_raw(std::string, std::uint64_t,
                                                                  SubscriberOptions);

public:
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client();
    vinc::Result<TopicPage> list_topics(std::string after = {});

    template <Message T> vinc::Result<Publisher<T>> advertise(std::string topic, PublisherOptions options = {}) {
        auto publisher = advertise_raw(std::move(topic), T::schema().hash, options);
        if (!publisher) {
            return std::unexpected(publisher.error());
        }

        return Publisher<T>(std::move(*publisher));
    }

    template <Message T> vinc::Result<Subscriber<T>> subscribe(std::string topic, SubscriberOptions options = {}) {
        auto subscriber = subscribe_raw(std::move(topic), T::schema().hash, options);
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }

        return Subscriber<T>(std::move(*subscriber));
    }
};

template <Message T> class Publisher {
    friend class Client;
    std::shared_ptr<detail::PublisherCore> core_;

    explicit Publisher(std::shared_ptr<detail::PublisherCore> publisher) : core_(std::move(publisher)) {
    }

public:
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    Publisher(Publisher&&) noexcept = default;

    Publisher& operator=(Publisher&& other) noexcept {
        if (this != &other) {
            close();
            core_ = std::move(other.core_);
        }

        return *this;
    }

    ~Publisher() {
        close();
    }

    // Reserve the next write slot. Overwrite waits for existing readers to finish their
    // framework copy; there is no contention timeout while the Publisher remains open.
    vinc::Result<WriteLoan<T>> get_next() {
        auto binding = detail::borrow(core_, T::schema());
        if (!binding) {
            return std::unexpected(binding.error());
        }

        auto loan = binding->loan;
        try {
            return WriteLoan<T>(std::move(*binding));
        } catch (const std::bad_alloc&) {
            detail::construction_failed(core_, loan);
            return std::unexpected(vinc::Error{ErrorCode::ResourceExhausted, ErrorOperation::Borrow});
        }
    }

    // Seal without a payload copy, assign publication metadata, and expose the slot.
    // Saved references into this loan must not be used after a successful commit.
    vinc::Result<void> commit(WriteLoan<T>& loan) {
        if (!loan.value_) {
            return std::unexpected(vinc::Error{ErrorCode::InvalidLoan, ErrorOperation::Commit});
        }

        auto result = detail::commit(core_, loan.loan_, message::detail::RecordAccess::binding(*loan.value_));
        if (result) {
            loan.value_.reset();
            loan.loan_.reset();
        }

        return result;
    }

    // Copy a local message into a reserved slot without consuming the source.
    // Reject an oversized packed image before reserving or modifying the ring buffer.
    vinc::Result<void> push(const T& value) {
        return detail::push(core_, message::detail::RecordAccess::binding(value));
    }

    PublisherStats stats() const {
        return detail::stats(core_);
    }

    void close() noexcept {
        detail::close(core_);
        core_.reset();
    }
};

template <Message T> class Subscriber {
    friend class Client;
    friend class detail::DispatchAccess;
    std::shared_ptr<detail::SubscriberCore> core_;

    explicit Subscriber(std::shared_ptr<detail::SubscriberCore> subscriber) : core_(std::move(subscriber)) {
    }

    vinc::Result<std::unique_ptr<T>> receive(bool latest) {
        auto result = detail::read(
            core_, latest, T::schema(), [](message::detail::Binding binding) -> std::unique_ptr<BaseMessage> {
                return std::unique_ptr<T>(new T(message::detail::RecordAccess::bind<T>(std::move(binding))));
            });
        if (!result) {
            return std::unexpected(result.error());
        }

        return std::unique_ptr<T>(static_cast<T*>(result->release()));
    }

public:
    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;
    Subscriber(Subscriber&&) noexcept = default;

    Subscriber& operator=(Subscriber&& other) noexcept {
        if (this != &other) {
            close();
            core_ = std::move(other.core_);
        }

        return *this;
    }

    ~Subscriber() {
        close();
    }

    SubscriberStats stats() const {
        return detail::stats(core_);
    }

    // Return the newest currently visible message, even if already returned before.
    // Do not advance callback progress; return NoPublishedData when no source has retained data.
    vinc::Result<std::unique_ptr<T>> get_last() {
        return receive(true);
    }

    void close() noexcept {
        detail::close(core_);
        core_.reset();
    }
};
}

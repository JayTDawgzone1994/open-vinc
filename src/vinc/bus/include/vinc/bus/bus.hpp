#pragma once
#include <vinc/message/generated.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <string>

namespace vinc::bus {
enum class ErrorCode : std::uint32_t {
    NoNewData = 1, NoPublishedData, OutstandingLoan, InvalidLoan, Closed,
    CapacityExceeded, TypeMismatch, ResourceExhausted, InvalidArgument,
    OperationInProgress, Protocol, Timeout, Io, InvalidData
};
struct Error { ErrorCode code; std::string message; };
template<class T> using Result = std::expected<T, Error>;
struct PublisherOptions { std::uint32_t slots = 2; std::uint64_t max_message_bytes = 1024 * 1024; };
enum class StartPosition { OldestRetained, NewOnly };
struct SubscriberOptions { StartPosition start = StartPosition::OldestRetained; };
struct Limits {
    std::uint32_t max_topics = 128, publishers_per_topic = 32, subscribers_per_topic = 128;
    std::uint64_t max_shared_bytes = 1024ULL * 1024 * 1024;
};
struct PublisherStats {
    std::uint32_t requested_slots = 0, effective_slots = 0;
    std::uint64_t max_message_bytes = 0, shared_bytes = 0, published = 0;
    std::uint64_t collisions = 0, total_wait_ns = 0, longest_wait_ns = 0, current_wait_ns = 0;
};
namespace detail {
struct PublisherCore; struct SubscriberCore; struct LoanCore; class RuntimeAccess;
struct LoanBinding { std::shared_ptr<LoanCore> loan; message::detail::Binding binding; };
Result<LoanBinding> borrow(const std::shared_ptr<PublisherCore>&, const message::detail::Schema&);
void construction_failed(const std::shared_ptr<PublisherCore>&, const std::shared_ptr<LoanCore>&) noexcept;
void abandon(const std::shared_ptr<LoanCore>&) noexcept;
bool usable(const std::shared_ptr<LoanCore>&) noexcept;
Result<void> commit(const std::shared_ptr<PublisherCore>&, const std::shared_ptr<LoanCore>&, message::detail::Binding);
Result<void> push(const std::shared_ptr<PublisherCore>&, message::detail::Binding);
void close(const std::shared_ptr<PublisherCore>&) noexcept;
void close(const std::shared_ptr<SubscriberCore>&) noexcept;
PublisherStats stats(const std::shared_ptr<PublisherCore>&);
using Factory = std::unique_ptr<BaseMessage>(*)(message::detail::Binding);
Result<std::unique_ptr<BaseMessage>> read(const std::shared_ptr<SubscriberCore>&, bool latest, const message::detail::Schema&, Factory);
}
template<class T> concept Message = std::is_base_of_v<message::GeneratedMessage<T>, T>;
template<Message T> class Publisher;
template<Message T> class Subscriber;

template<Message T> class WriteLoan {
    friend class Publisher<T>;
    std::shared_ptr<detail::LoanCore> loan_;
    std::unique_ptr<T> value_;
    WriteLoan(detail::LoanBinding b) : loan_(std::move(b.loan)),
        value_(new T(message::detail::RecordAccess::bind<T>(std::move(b.binding)))) {}
public:
    WriteLoan(const WriteLoan&) = delete;
    WriteLoan& operator=(const WriteLoan&) = delete;
    WriteLoan(WriteLoan&&) noexcept = default;
    WriteLoan& operator=(WriteLoan&& other) noexcept {
        if (this != &other) { detail::abandon(loan_); value_.reset(); loan_ = std::move(other.loan_); value_ = std::move(other.value_); }
        return *this;
    }
    ~WriteLoan() { detail::abandon(loan_); }
    T* operator->() { if (!value_ || !detail::usable(loan_)) throw message::AccessError({message::ErrorCode::Closed}); return value_.get(); }
    T& operator*() { return *operator->(); }
};

class Client {
    friend class detail::RuntimeAccess;
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit Client(std::shared_ptr<Impl>);
    Result<std::shared_ptr<detail::PublisherCore>> advertise_raw(std::string, std::uint64_t, PublisherOptions);
    Result<std::shared_ptr<detail::SubscriberCore>> subscribe_raw(std::string, std::uint64_t, SubscriberOptions);
public:
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client();
    template<Message T> Result<Publisher<T>> advertise(std::string topic, PublisherOptions options = {}) {
        auto p = advertise_raw(std::move(topic), T::schema().hash, options);
        if (!p) return std::unexpected(p.error());
        return Publisher<T>(std::move(*p));
    }
    template<Message T> Result<Subscriber<T>> subscribe(std::string topic, SubscriberOptions options = {}) {
        auto s = subscribe_raw(std::move(topic), T::schema().hash, options);
        if (!s) return std::unexpected(s.error());
        return Subscriber<T>(std::move(*s));
    }
};

template<Message T> class Publisher {
    friend class Client;
    std::shared_ptr<detail::PublisherCore> core_;
    explicit Publisher(std::shared_ptr<detail::PublisherCore> p) : core_(std::move(p)) {}
public:
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    Publisher(Publisher&&) noexcept = default;
    Publisher& operator=(Publisher&& other) noexcept { if (this != &other) { close(); core_ = std::move(other.core_); } return *this; }
    ~Publisher() { close(); }
    Result<WriteLoan<T>> get_next() {
        auto b = detail::borrow(core_, T::schema());
        if (!b) return std::unexpected(b.error());
        auto loan = b->loan;
        try { return WriteLoan<T>(std::move(*b)); }
        catch (const std::bad_alloc&) { detail::construction_failed(core_, loan); return std::unexpected(Error{ErrorCode::ResourceExhausted, "allocate message facade"}); }
    }
    Result<void> commit(WriteLoan<T>& loan) {
        if (!loan.value_) return std::unexpected(Error{ErrorCode::InvalidLoan, "empty or consumed loan"});
        auto r = detail::commit(core_, loan.loan_, message::detail::RecordAccess::binding(*loan.value_));
        if (r) { loan.value_.reset(); loan.loan_.reset(); }
        return r;
    }
    Result<void> push(const T& value) { return detail::push(core_, message::detail::RecordAccess::binding(value)); }
    PublisherStats stats() const { return detail::stats(core_); }
    void close() noexcept { detail::close(core_); core_.reset(); }
};

template<Message T> class Subscriber {
    friend class Client;
    std::shared_ptr<detail::SubscriberCore> core_;
    explicit Subscriber(std::shared_ptr<detail::SubscriberCore> s) : core_(std::move(s)) {}
    Result<std::unique_ptr<T>> receive(bool latest) {
        auto r = detail::read(core_, latest, T::schema(), [](message::detail::Binding b) -> std::unique_ptr<BaseMessage> {
            return std::unique_ptr<T>(new T(message::detail::RecordAccess::bind<T>(std::move(b))));
        });
        if (!r) return std::unexpected(r.error());
        return std::unique_ptr<T>(static_cast<T*>(r->release()));
    }
public:
    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;
    Subscriber(Subscriber&&) noexcept = default;
    Subscriber& operator=(Subscriber&& other) noexcept { if (this != &other) { close(); core_ = std::move(other.core_); } return *this; }
    ~Subscriber() { close(); }
    Result<std::unique_ptr<T>> next() { return receive(false); }
    Result<std::unique_ptr<T>> get_last() { return receive(true); }
    void close() noexcept { detail::close(core_); core_.reset(); }
};
}

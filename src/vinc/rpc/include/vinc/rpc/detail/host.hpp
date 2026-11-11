#pragma once
#include <vinc/rpc/detail/access.hpp>
#include <vinc/control/channel.hpp>
#include <condition_variable>
#include <map>

namespace vinc::rpc::detail {
class Host final : public Access {
public:
    explicit Host(std::shared_ptr<control::Session> session);
    void stop() noexcept;
    Result<void> receive(control::Frame frame);
    Result<void> progress();
    bool ready() const noexcept;
    bool living() const noexcept override;
    Result<std::uint64_t> reserve_request() override;
    Result<control::Bytes> exchange(control::Frame request) override;
    Result<void> send(control::Frame request) override;
    std::deque<control::Frame> take() override;
    Result<std::unique_ptr<BlockingCall>> start_call(
        control::Frame request, std::shared_ptr<CallReservation> reservation) override;

private:
    std::shared_ptr<control::Session> session_;
    std::atomic<bool> stopped_ = false;
    std::atomic<bool> ready_ = false;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t next_request_ = 1;

    struct Pending {
        bool done = false;
        control::Bytes bytes;
    };

    std::map<std::uint64_t, std::shared_ptr<Pending>> pending_;
    std::deque<control::Frame> input_;
    std::deque<control::Frame> output_;

    struct Synchronous {
        std::uint64_t id = 0;
        std::shared_ptr<CallReservation> reservation;
        std::optional<control::Frame> grant;
        std::optional<control::Frame> reply;
        std::optional<Transfer> transfer;
        bool grant_seen = false;
        bool reply_seen = false;
        bool submitted = false;
        bool abandoned = false;
    };
    class Waiter;
    std::map<std::uint64_t, std::shared_ptr<Synchronous>> synchronous_;

    Result<void> send_locked(control::Frame request);
    Result<void> receive_synchronous(Synchronous& call, control::Frame frame);
    Result<void> retire_synchronous();
    void abandon(const std::shared_ptr<Synchronous>& call) noexcept;
    Result<control::Frame> wait_event(Synchronous& call, bool reply, Deadline deadline);
    Result<RawResult> wait_call(Synchronous& call, std::span<const std::byte> image, Deadline deadline);
};
}

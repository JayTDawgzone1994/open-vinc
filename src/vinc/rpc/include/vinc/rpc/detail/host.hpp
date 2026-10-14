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
};
}

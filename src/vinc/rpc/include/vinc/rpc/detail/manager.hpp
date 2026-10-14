#pragma once
#include <vinc/rpc/detail/storage.hpp>
#include <vinc/control/channel.hpp>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

namespace vinc::rpc::detail {
class RpcManager {
    friend struct ManagerTests;

public:
    explicit RpcManager(std::shared_ptr<vinc::detail::ResourceNames> names);
    ~RpcManager();
    void admit(std::uint64_t mid, std::string name, std::shared_ptr<control::Session> session);
    void exited(std::uint64_t mid);
    void stopping(std::uint64_t mid);
    Result<void> submit(std::uint64_t mid, control::Frame frame);

private:
    struct Peer {
        std::uint64_t mid = 0;
        std::string name;
        std::shared_ptr<control::Session> session;
        std::atomic<bool> alive = true;
        std::atomic<bool> accepting = true;
        bool recovered = false;
        std::uint64_t last_request = 0;
        std::deque<control::Frame> output;
    };
    enum class Phase {
        Granted,
        Queued,
        Reply
    };

    struct Slot {
        std::uint64_t token = 0;
        std::shared_ptr<Peer> caller;
        std::uint64_t request = 0;
        Phase phase = Phase::Granted;
    };

    struct Service {
        std::uint64_t id = 0;
        std::shared_ptr<Peer> owner;
        std::string name;
        std::uint64_t request_hash = 0;
        std::uint64_t response_hash = 0;
        ServiceOptions options;
        std::unique_ptr<Pool> pool;
        std::vector<Slot> slots;
    };

    struct Input {
        std::shared_ptr<Peer> peer;
        control::Frame frame;
    };

    std::shared_ptr<vinc::detail::ResourceNames> names_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::uint64_t, std::shared_ptr<Peer>> peers_;
    std::deque<Input> input_;
    std::map<std::uint64_t, Service> services_;
    std::uint64_t next_service_ = 1;
    std::uint64_t next_token_ = 1;
    std::uint64_t bytes_ = 0;
    std::size_t occupied_ = 0;
    std::jthread worker_;
    void run(std::stop_token stop);
    void execute(Input input);
    Result<control::Bytes> register_service(const std::shared_ptr<Peer>& peer, const control::Frame& frame);
    Result<control::Bytes> acquire(const std::shared_ptr<Peer>& peer, const control::Frame& frame);
    Result<void> transition(const std::shared_ptr<Peer>& peer, const control::Frame& frame);
    void reply(const std::shared_ptr<Peer>& peer, Operation operation, std::uint64_t request,
               Result<control::Bytes> result);
    void flush(Peer& peer);
    void recover(Peer& peer);
    void free_slot(Slot& slot);
    Transfer descriptor(const Service& service, std::size_t index) const;
};
}

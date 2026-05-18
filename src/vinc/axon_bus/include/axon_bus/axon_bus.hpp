#pragma once
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <shared_mutex>
#include <thread>

#include <axon_bus/axon_define.hpp>
#include <axon_bus/object_pool.hpp>
#include <axon_bus/work_queue.hpp>

#include <base_message.hpp>
#include <module/base_module.hpp>

namespace vinc 
{

class AxonBus
{
public:

    using Handler = void(*)(const BaseMessage*, void*);

    static AxonBus& get();
    ~AxonBus() = default;

    // 禁止拷貝
    AxonBus(const AxonBus&) = delete;
    AxonBus& operator=(const AxonBus&) = delete;

    template <typename DerivedMsg, typename... Args>
    auto make_message(Args&&... args);

    uint64_t low_level_subscribe(const std::string& topic, Handler handler, void* userData);
  
    void unsubscribe(const std::string& topic, uint64_t id);

    void low_level_publish(const std::string& topic, std::shared_ptr<BaseMessage> msg);
    
    // 在系統啟動時呼叫，喚醒發動機
    void start_engine();

    // 系統關閉時呼叫，安全熄火
    void stop_engine();

    void worker_loop();

private:
    AxonBus();

    template <typename T, typename M, auto Method>
    static void Trampoline(const BaseMessage* msg, void* uData);

    WorkQueue _work_queue;

    std::shared_mutex _sub_mutex;
    std::unordered_map<std::string, std::vector<InternalSub>> _sub_map;

    LockFreeObjectPool<vinc::BaseMessage, 256> _msg_pool{1024};
    LockFreeObjectPool<WorkTask, 128> _task_pool{4096};

    std::atomic<bool> _running;
    std::atomic<uint64_t> _id_counter{1}; // 從 1 開始，0 可以保留作為無效 ID

    std::vector<std::thread> _worker_threads;

};

} //namespace vinc
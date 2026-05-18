#pragma once
#include <iostream>
#include <memory>
#include <vector>

#include <axon_bus/axon_bus.hpp>

namespace vinc {

class AxonBus; // 前向宣告

class Subscriber 
{
public:
    Subscriber() = default;
    
    ~Subscriber() { reset(); }

    // 支援移動，不支援拷貝
    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;
    Subscriber(Subscriber&&) = delete;
    Subscriber& operator=(Subscriber&&) = delete;

    /**
     * @brief 👈 主動訂閱功能！
     * 語法：sub.subscribe("TOPIC", this, &MyClass::onMsg);
     */
    template <typename T, typename M>
    void subscribe(const std::string& topic, T* instance, void (T::*method)(const M*)) 
    {
        // 1. 如果之前有訂閱過別的，先清理掉，確保一個物件只管一個訂閱
        reset(); 

        _topic = topic;

        // 2. 在這裡建立並直接持有 Thunk 記憶體
        auto thunk = std::make_unique<SubscriptionThunk<T, M>>(instance, method);
        void* raw_thunk_ptr = thunk.get();
        _thunk_storage = std::move(thunk); 

        // 3. 建立零開銷的 Lambda 轉接頭
        auto lambda_handler = [](const BaseMessage* msg, void* uData) 
        {
            auto* thunk_ptr = static_cast<SubscriptionThunk<T, M>*>(uData);
            (thunk_ptr->instance->*(thunk_ptr->method))(static_cast<const M*>(msg));
        };

        // 4. 主動向單例 Bus 註冊，並拿回身份證字號
        _id = AxonBus::get().low_level_subscribe(_topic, lambda_handler, raw_thunk_ptr);
    }

    void reset() 
    {
        if (_bus && !_topic.empty()) 
        {
            _bus->unsubscribe(_topic, _id); // 1. 先去 Bus 把 InternalSub 拔掉
            _bus = nullptr;
            _topic.clear();
        }
        // 2. 函式結束後，_thunk_storage 會自動解構，釋放記憶體
    }

private:
    //friend class AxonBus;

    AxonBus* _bus = nullptr;
    std::string _topic;
    uint64_t _id = 0;
    std::unique_ptr<ISubscriptionThunk> _thunk_storage; // 記憶體在這裡！
};


} // namespace vinc
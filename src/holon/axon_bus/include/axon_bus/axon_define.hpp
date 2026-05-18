#pragma once

#include <string>
#include <memory>

#include <axon_bus/object_pool.hpp>
#include <base_message.hpp>

namespace holon 
{

struct WorkTask 
{
    using Handler = void(*)(const BaseMessage*, void*);
    
    Handler handler = nullptr;
    void* userData = nullptr;
    std::shared_ptr<BaseMessage> msg_sp;

    virtual ~WorkTask() = default; 
};

// 橡皮擦基底類別（留在外面給 Subscriber 拿著）
struct ISubscriptionThunk 
{
    virtual ~ISubscriptionThunk() = default;
};

template <typename T, typename M>
struct SubscriptionThunk : public ISubscriptionThunk 
{
    T* instance;
    void (T::*method)(const M*);
    SubscriptionThunk(T* inst, void (T::*m)(const M*)) : instance(inst), method(m) {}
};

struct InternalSub 
{
    using Handler = void(*)(const BaseMessage*, void*);
    Handler handler;
    void* userData; // 這裡依然指向 Thunk，但 Bus 不負責它的生死
    uint64_t id;
};

}

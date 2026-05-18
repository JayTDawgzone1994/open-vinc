#pragma once

#include <iostream>
#include <memory>
#include <string>

#include <axon_bus/axon_bus.hpp>

// Publisher.hpp
namespace holon {

template <typename M>
class Publisher {
public:
    // 引入全域統一的訊息回收清潔工型別
    using MessageDeleter = typename holon::LockFreeObjectPool<holon::BaseMessage, 256>::Deleter;

    Publisher() = default;
    ~Publisher() = default;

    // 封印複製與移動
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    Publisher(Publisher&&) = delete;
    Publisher& operator=(Publisher&&) = delete;

    void init(const std::string& topic) {
        _topic = topic;
    }

    /**
     * @brief 👈 改用左值引用 &
     * 使用者傳入原本的 unique_ptr 變數即可，函式內部會自動把它抽乾
     */
    uint8_t publish(std::unique_ptr<M, MessageDeleter>& msg_ptr) 
    { // 👈 關鍵在這裡的 &
        if (_topic.empty() || !msg_ptr)
        {
            return -1; // 發布失敗，主題未設定或訊息無效
        } 

        // 👈 在內部執行 move！把使用者那邊的所有權徹底「偷」過來
        // 因為 msg_ptr 是引用，這行會直接把使用者外面的那個 unique_ptr 清空成 nullptr
        std::shared_ptr<BaseMessage> shared_msg = std::move(msg_ptr);

        // 丟給 Bus 進行底層派發
        AxonBus::get().low_level_publish(_topic, std::move(shared_msg));

        return 0; // 發布成功
    }

private:
    std::string _topic;
};

} // namespace holon
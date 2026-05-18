#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace vinc
{

template <typename BaseT, size_t MaxSize = 256>
class LockFreeObjectPool 
{
public:
    // 坑位結構 (16位元組對齊)
    struct Node 
    {
        alignas(16) std::byte buffer[MaxSize]; 
        Node* next;
    };

    // 64-bit 標籤指標 (原生硬體無鎖支援)
    struct alignas(8) CompressedTaggedPointer 
    {
        uint64_t raw_value = 0; // 48位元指標 + 16位元 Tag

        template <typename T>
        T* get_pointer() const {
            return reinterpret_cast<T*>(raw_value & 0x0000FFFFFFFFFFFFULL);
        }

        uint16_t get_tag() const {
            return static_cast<uint16_t>(raw_value >> 48);
        }

        template <typename T>
        void pack(T* ptr, uint16_t tag) {
            uint64_t ptr_val = reinterpret_cast<uint64_t>(ptr);
            raw_value = (ptr_val & 0x0000FFFFFFFFFFFFULL) | (static_cast<uint64_t>(tag) << 48);
        }
    };

    // 自動回收機器人
    struct Deleter 
    {
        LockFreeObjectPool* pool;
        void operator()(BaseT* ptr) const;
    };

    using PtrType = std::unique_ptr<BaseT, Deleter>;

    explicit LockFreeObjectPool(size_t initial_size);
    ~LockFreeObjectPool() = default;

    // 禁止拷貝
    LockFreeObjectPool(const LockFreeObjectPool&) = delete;
    LockFreeObjectPool& operator=(const LockFreeObjectPool&) = delete;

    Node* acquire_raw_node();
    void recycle(Node* node);

private:
    void grow(size_t size);
    Node* expand_and_acquire_raw();

    // 👈 修正點：移除舊的 {nullptr, 0} 初始化，改用 {} 讓內部 raw_value 預設為 0
    alignas(64) std::atomic<CompressedTaggedPointer> _head{ CompressedTaggedPointer{} };
    
    std::vector<std::unique_ptr<Node[]>> _memory_blocks;
    std::mutex _expansion_mutex;
    size_t _grow_size;
};

} // namespace vinc
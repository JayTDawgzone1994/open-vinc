#include <axon_bus/object_pool.hpp>
#include <base_message.hpp>

namespace vinc
{

// --- Deleter 實作 ---
template <typename BaseT, size_t MaxSize>
void LockFreeObjectPool<BaseT, MaxSize>::Deleter::operator()(BaseT* ptr) const 
{
    if (ptr) 
    {
        // 執行虛擬解構
        ptr->~BaseT(); 
        // 因為 buffer 是 Node 的第一個成員，地址完全相同，可以直接強轉
        Node* node = reinterpret_cast<Node*>(ptr);
        pool->recycle(node);
    }
}

// --- 建構子 ---
template <typename BaseT, size_t MaxSize>
LockFreeObjectPool<BaseT, MaxSize>::LockFreeObjectPool(size_t initial_size) 
    : _grow_size(initial_size)
{
    static_assert(std::atomic<CompressedTaggedPointer>::is_always_lock_free, 
                "CompressedTaggedPointer must be lock-free!");
    // 👈 修正點：刪除了原本卡住編譯的 128-bit static_assert
    grow(initial_size);
}

// --- Acquire (出池) ---
template <typename BaseT, size_t MaxSize>
typename LockFreeObjectPool<BaseT, MaxSize>::Node* LockFreeObjectPool<BaseT, MaxSize>::acquire_raw_node() 
{
    // 👈 修正點：全面改用 CompressedTaggedPointer 接口
    CompressedTaggedPointer old_head = _head.load(std::memory_order_acquire);
    CompressedTaggedPointer new_head;

    while (true) 
    {
        Node* old_node = old_head.template get_pointer<Node>();
        if (!old_node) break; // 池子空了，跳出迴圈去擴容

        // 核心無鎖互斥：抓取當前節點的下一個指標，並將版本號加 1 驅散 ABA 幽靈
        Node* next_node = old_node->next;
        new_head.pack(next_node, old_head.get_tag() + 1);

        if (_head.compare_exchange_weak(old_head, new_head,
                                       std::memory_order_acq_rel, 
                                       std::memory_order_acquire)) {
            return old_node; // 搶截成功，順利出池
        }
        // 若被併發執行緒捷足先登，old_head 會被自動更新，迴圈會繼續重試
    }
    return expand_and_acquire_raw();
}

// --- Recycle (還池) ---
template <typename BaseT, size_t MaxSize>
void LockFreeObjectPool<BaseT, MaxSize>::recycle(Node* node) 
{
    if (!node) return;

    CompressedTaggedPointer old_head = _head.load(std::memory_order_relaxed);
    CompressedTaggedPointer new_head;

    do {
        // 將當前頭部解包後的原始地址，掛在你要還回去的 node 的 next 屁股後面
        node->next = old_head.template get_pointer<Node>();
        
        // 打包新頭部：指針換成目前要還的 node，Tag 繼續累加
        new_head.pack(node, old_head.get_tag() + 1);
        
    } while (!_head.compare_exchange_weak(old_head, new_head,
                                           std::memory_order_release, 
                                           std::memory_order_relaxed));
}

// --- Grow ---
template <typename BaseT, size_t MaxSize>
void LockFreeObjectPool<BaseT, MaxSize>::grow(size_t size) 
{
    auto new_block = std::make_unique<Node[]>(size);
    for (size_t i = 0; i < size; ++i) {
        recycle(&new_block[i]);
    }
    _memory_blocks.push_back(std::move(new_block));
}

// --- Expand ---
template <typename BaseT, size_t MaxSize>
typename LockFreeObjectPool<BaseT, MaxSize>::Node* LockFreeObjectPool<BaseT, MaxSize>::expand_and_acquire_raw() 
{
    std::lock_guard<std::mutex> lock(_expansion_mutex);
    CompressedTaggedPointer current_head = _head.load(std::memory_order_acquire);
    
    // 解包檢查，如果被別的執行緒搶先擴容了，就不用重複做
    if (current_head.template get_pointer<Node>() == nullptr) {
        grow(_grow_size);
    }
    return acquire_raw_node();
}

// =================================================================
// 顯式實例化
// =================================================================
template class LockFreeObjectPool<BaseMessage, 256>;

} // namespace vinc
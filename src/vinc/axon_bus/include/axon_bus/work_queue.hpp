#pragma once
#include <vector>
#include <mutex>
#include <semaphore>

#include <axon_bus/axon_define.hpp> // 包含前面定義的 TaskPtr

namespace vinc 
{

using TaskDeleter = typename LockFreeObjectPool<WorkTask, 128>::Deleter;
using TaskPtr = std::unique_ptr<WorkTask, TaskDeleter>;

class WorkQueue 
{
public:
    // 預留 512 個任務坑位，防止 100Hz 突發流量塞爆
    explicit WorkQueue(size_t capacity = 512) 
        : _capacity(capacity), _head(0), _tail(0), _size(0), _semaphore(0) {
        _queue.resize(_capacity);
    }

    ~WorkQueue() = default;

    // 封印複製與移動
    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;

    /**
     * @brief 推進新任務 (由 AxonBus::low_level_publish 呼叫)
     */
    void enqueue(TaskPtr task) 
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_size >= _capacity) [[unlikely]] {
                // 航電系統防禦性編程：佇列滿了，直接丟棄舊任務（或印 Log 警告）
                return; 
            }
            
            // 完美移動所有權，不產生任何複製
            _queue[_tail] = std::move(task);
            _tail = (_tail + 1) % _capacity;
            _size++;
        }
        // 👈 C++20 號誌鎖釋放：通知 Worker 執行緒「有新獵物了」
        _semaphore.release(); 
    }

    /**
     * @brief 拔出任務 (由後台 Worker Thread 呼叫，沒任務時會在這裡休眠)
     */
    TaskPtr dequeue() {
        // 👈 阻塞等待：沒任務時，執行緒會在這裡完全睡眠，不吃任何 CPU！
        _semaphore.acquire(); 

        std::lock_guard<std::mutex> lock(_mutex);
        
        TaskPtr task = std::move(_queue[_head]);
        _head = (_head + 1) % _capacity;
        _size--;

        return task;
    }

private:
    std::vector<TaskPtr> _queue; // 固定大小的容器
    const size_t _capacity;
    size_t _head;
    size_t _tail;
    size_t _size;

    std::mutex _mutex;
    std::counting_semaphore<512> _semaphore; // C++20 極速號誌鎖
};

} // namespace vinc
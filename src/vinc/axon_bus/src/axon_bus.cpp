#include <axon_bus/axon_bus.hpp>

#include <axon_bus/subscriber.hpp>

namespace vinc 
{

AxonBus& AxonBus::get() 
{
    static AxonBus instance;
    return instance;
}

template <typename DerivedMsg, typename... Args>
auto AxonBus::make_message(Args&&... args) {
    // 編譯期安全檢查
    static_assert(std::is_base_of_v<vinc::BaseMessage, DerivedMsg>, "Must derive from BaseMessage");
    static_assert(sizeof(DerivedMsg) <= 256, "Derived message too large for pool buffer!");

    // 1. 借出坑位
    auto node = _msg_pool.acquire_raw_node();
    
    // 2. 在坑位上 Placement New 構造物件
    // 這會正確設定虛擬函式表 (vtable)
    DerivedMsg* msg_ptr = new (node->buffer) DerivedMsg(std::forward<Args>(args)...);

    // 3. 返回封裝好的 unique_ptr
    using Deleter = typename vinc::LockFreeObjectPool<vinc::BaseMessage, 256>::Deleter;
    return std::unique_ptr<DerivedMsg, Deleter>(msg_ptr, Deleter{&_msg_pool});
}

/**
 * @brief 低階訂閱 (冷路徑：初始化期執行)
 * 使用 unique_lock 上寫鎖，不允許其他人在此時讀寫
 */
uint64_t AxonBus::low_level_subscribe(const std::string& topic, Handler handler, void* userData) {
    std::unique_lock<std::shared_mutex> lock(_sub_mutex); // 👈 獨佔寫鎖
    
    uint64_t new_id = _id_counter.fetch_add(1, std::memory_order_relaxed);
    
    // 這裡直接搬移或複製平坦結構體，速度極快
    _sub_map[topic].push_back(InternalSub{handler, userData, new_id});
    
    return new_id;
}

void AxonBus::unsubscribe(const std::string& topic, uint64_t id) 
{
    std::unique_lock<std::shared_mutex> lock(_sub_mutex); // 👈 獨佔寫鎖
    
    auto it = _sub_map.find(topic);
    if (it != _sub_map.end()) 
    {
        auto& subs = it->second;
        
        // 運用 std::remove_if 在連續記憶體中抹除對應 ID 的資料
        subs.erase(std::remove_if(subs.begin(), subs.end(),
            [id](const InternalSub& s) { return s.id == id; }), subs.end());
            
    }
}

/**
 * @brief 低階發布 (熱路徑：100Hz 主迴圈)
 * 使用 shared_lock 上讀鎖，允許多個模組並行 publish，彼此完全不卡頓！
 */
void AxonBus::low_level_publish(const std::string& topic, std::shared_ptr<BaseMessage> msg) {
    std::shared_lock<std::shared_mutex> lock(_sub_mutex); // 👈 共享讀鎖：並行關鍵！

    auto it = _sub_map.find(topic);
    if (it == _sub_map.end()) return; // 沒人訂閱，原地解散

    // 完美的連續記憶體遍歷，硬體預取器（Prefetcher）會直接把這一排通訊錄拉進 CPU Cache
    const auto& subs = it->second;
    for (const auto& sub : subs) 
    {
        
        // 1. 從無鎖 WorkTask 池子裡挖出一塊生記憶體（Raw Node）
        auto* node = _task_pool.acquire_raw_node(); 
        if (!node) [[unlikely]] {
            // 處理池子爆炸的極端狀況（航電系統防禦性編程）
            continue; 
        }
        
        // 2. 使用 Placement New 在連續的池子記憶體上構造 WorkTask 物件
        WorkTask* task_ptr = new (node->buffer) WorkTask();
        
        // 3. 填入通訊錄三劍客：Code, Context, Data
        task_ptr->handler = sub.handler;
        task_ptr->userData = sub.userData;
        task_ptr->msg_sp = msg; // 👈 關鍵：shared_ptr 複製，內部計數器原子加 1

        // 4. 用池子專屬的 Deleter 打包成 unique_ptr
        //    這樣當 Worker 線程處理完任務銷毀時，會呼叫池子的回收函式，而不是 free()
        using TaskDeleter = typename LockFreeObjectPool<WorkTask, 128>::Deleter;
        auto task_uptr = std::unique_ptr<WorkTask, TaskDeleter>(task_ptr, TaskDeleter{&_task_pool});

        // 5. 推進任務佇列，彈射給 Worker 執行緒去異步執行！
        _work_queue.enqueue(std::move(task_uptr)); // 👈 完美移動，沒有任何複製
    }
}

void AxonBus::start_engine() 
{
    if (_running.exchange(true)) return; // 防止重複啟動
  
    
    uint32_t thread_count = std::thread::hardware_concurrency();
    // 3. 👈 防禦性安全網：如果 OS 查不到回傳 0，立刻強制採用你設定的預設 4 核
    if (thread_count == 0) [[unlikely]] 
    {
        thread_count = 4;
    }

    _worker_threads.reserve(thread_count);
    
    // 5. 依據最終計算出的核心數，喚醒對應數量的 Worker 執行緒
    for (size_t i = 0; i < thread_count; ++i) 
    {
        _worker_threads.emplace_back(&AxonBus::worker_loop, this);
    }
}

void AxonBus::stop_engine() 
{
    if (!_running.exchange(false)) return;
    
    // 塞入一個空任務來喚醒可能正在睡覺的執行緒
    low_level_publish("SHUTDOWN_SIGNAL", nullptr); 
    
    for (auto& thread : _worker_threads) 
    {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void AxonBus::worker_loop() 
{
    while (_running.load(std::memory_order_relaxed)) 
    {
        // 1. 從佇列裡拔任務（沒任務就原地睡覺，不浪費電池）
        TaskPtr task = _work_queue.dequeue();
        
        // 安全防禦
        if (!task || task->handler == nullptr) continue;

        // 2. 彈射！執行使用者註冊的成員函式轉接頭
        //    傳入 Data (msg_sp) 與 Context (userData)
        task->handler(task->msg_sp.get(), task->userData);

        // 3. 👈 驚天動地的閉環魔術在這裡發生：
        //    當這一輪迴圈結束，task 變數離開作用域（Scope），
        //    std::unique_ptr 的解構子被觸發。
        //    它會自動調用 TaskDeleter，把這一塊 WorkTask 的生記憶體
        //    完美、無鎖地丟回 _task_pool 物件池中！
        //    
        //    同理，如果 task->msg_sp 的引用計數此時歸零，
        //    訊息本體也會被自動丟回 _message_pool！
    }
}

template <typename T, typename M, auto Method>
void AxonBus::Trampoline(const BaseMessage* msg, void* uData) 
{
    (static_cast<T*>(uData)->*Method)(static_cast<const M*>(msg));
}

} //namespace vinc
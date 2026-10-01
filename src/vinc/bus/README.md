# Shared Memory Message Bus

`vinc::bus` 接上 OSAL SharedMemory、Ribosome 生成訊息及獨立程序 Module。
模組在 `on_start()` 之後透過 `BaseModule::bus()` 取得 Client；建構子內尚不可使用。
每個 Publisher 擁有獨立 Ring，同一 topic 可有多個 Publisher。大量訊息留在共享記憶體，
Socket 控制通道只交換註冊、退訂與 Publisher 增減資訊。

這是第一版 pull API。舊 `axon_bus` 保留為 legacy，未自動轉接，也未加入每筆訊息的 callback／Socket 通知。

## 三條使用路徑

以下程式位於 `BaseModule` 的 hook；`Frame` 是測試 fixture 的生成訊息，正式專案可替換成自己的 `.msg`。
範例省略錯誤分支的業務處理；每個 Result 都需要檢查。

```cpp
auto registered = bus().advertise<vinc::Frame>("frames", {
    .slots = 4, .max_message_bytes = 4 * 1024 * 1024
});
if (!registered) { /* registered.error() */ return; }
auto pub = std::move(*registered);

// 本機訊息可成長；push 檢查容量後把有效映像複製進共享 slot。
auto local = vinc::make_message<vinc::Frame>();
local->data().sequence = 42;
auto sent = pub.push(*local);

// 借到共享 slot，填寫後提交；commit 不複製整筆 payload。
auto borrowed = pub.get_next();
if (borrowed) {
    auto& loan = *borrowed;
    loan->data().sequence = 43;
    auto committed = pub.commit(loan);
    // 成功後 loan 已消耗，不可再存取。
}

auto registered_sub = bus().subscribe<vinc::Frame>("frames");
if (!registered_sub) { /* registered_sub.error() */ return; }
auto sub = std::move(*registered_sub);
auto received = sub.next(); // Result<unique_ptr<Frame>>，完整映像複製一次到本機
if (received) {
    auto msg = std::move(*received);
    const auto count = msg->get_topic_count();
    // msg 可修改；不再占用來源 slot，Publisher 退出也不影響此副本。
}
```

正式模組通常把 Publisher／Subscriber 存成成員，於 `on_start` 建立。
生成訊息 header 的 CMake 相依由 `target_link_libraries(target PRIVATE vinc_messages)` 提供。

## 容量與借用契約

- `slots` 預設 2，輸入 1 自動變成 2；0 拒絕。沒有額外 N+1 slot。
- `max_message_bytes` 是單筆完整訊息映像上限，包含 header、descriptor、索引、資料及訊息內預留容量。
  例如 4 筆 × 4 MiB 有 16 MiB payload 空間，另加 Ring 控制資料。slot 起點補齊 8 bytes；
  byte 上限本身不要求 8 的倍數，不會因補齊而放寬可寫容量。
- `push` 的本機來源不變；先計算緊密映像，超限回 `CapacityExceeded`，也不淘汰舊資料。
  成功保留來源，寫入時移除來源未使用的預留容量。配置失敗等其他錯誤不承諾保留所有舊歷史，
  但未完成的新訊息不會發布。
- `get_next` 借出時淘汰最舊可重用 slot，保留上一筆已發布的最新資料。
  共享訊息 resize／reserve 等操作立即受該筆 byte 上限限制；失敗保留該欄位操作前內容。
- 每個 Publisher 同時最多一筆 loan。未 commit 再借或 push 回 `OutstandingLoan`。
  丟棄 loan **不會**解除此狀態；這是明確的使用錯誤，需關閉該 Publisher 才結束。
- loan 為 move-only；commit 只接受該 Publisher 借出的物件，成功後撤銷寫入並消耗 loan。
  複製 loan 內的生成訊息／陣列會取得獨立本機資料，不能把副本當作原 loan 提交。
- `commit` 驗證既有映像、配置本機 metadata，再取得時間與計數並原子發布。
  「zero-copy commit」指不複製／搬移整筆 payload；填寫時的 resize、布局重排、驗證仍有成本。

## 讀取、排序與識別

| 操作 | 行為 |
| --- | --- |
| `next()` | 每個 Publisher 找最舊未讀且仍保留的資料，再合併目前可見候選；成功才推進該來源游標 |
| `get_last()` | 取得目前可見候選中的最新資料，可重複取得同一筆；不改變 `next()` 進度 |
| 無新資料 | `next()` 立即回 `NoNewData`，不等待未來發布 |
| 尚無 Publisher／任何可讀資料 | `get_last()` 立即回 `NoPublishedData` |
| 讀者落後 | 被覆蓋的歷史跳過，從該來源仍保留的最舊資料繼續 |

預設新訂閱從 `StartPosition::OldestRetained` 開始。
`SubscriberOptions{.start = StartPosition::NewOnly}` 略過註冊時各 Publisher 已發布的資料；
之後新增 Publisher 從其第一筆開始。`get_last` 不受 NewOnly 起點限制。

跨 Publisher 比較 OS 單調時間戳，相同時間取較早註冊的 Publisher；同一 Publisher 維持自身發布順序。
只比較這次掃描所觀察到的已提交候選，不提供所有 Publisher 的原子快照或全域嚴格 FIFO。
取得時間後被 OS 暫停的 Publisher，可能稍後才讓較舊時間戳可見；不等待或回頭重排已交付訊息。

時間戳單位為 nanoseconds，同一主機內比較，**不是 Unix 時間或 nanosecond 精度承諾**。
框架填入 `topic_name()`、`publisher_name()`、`publisher_instance()`、
`get_message_timestamp()`、`get_topic_count()`；使用者沒有 metadata setter。
topic count 在同一 topic 存活期間由各 Publisher 共用 atomic 計數器配置，可能因失敗有缺號，
不是排序依據。topic 無訂閱也無 Publisher 後釋放，重建時 count 重新開始；
Publisher instance ID 在同一 ModuleManager 生命週期不重用，不能跨中央重啟當持久識別。
slot 的 generation 另行防止重用造成 ABA；計數／generation 耗盡時拒絕，不循環回零。

## 碰撞、生命週期與並行

讀者先在本機配置接收空間，再保護來源、執行一次 `memcpy`、解除保護，最後驗證本機副本與建構 class。
保護期間沒有使用者 callback。寫入者關閉該 slot 對新讀者的入口，等待既有複製完成。
正常碰撞不設逾時；使用者已選擇這項行為。執行緒暫停／系統排程仍可能造成長時間等待。

`stats()` 提供 `collisions`、完成等待的 `total_wait_ns`／`longest_wait_ns`，
以及尚在等待的 `current_wait_ns`。一次 slot 回收等待計為一次碰撞，不是每次輪詢加一。
統計是近似快照，各欄位非一次原子讀取；時間／次數採飽和累加。

heartbeat 與控制收發由獨立執行緒維護，不受 Bus writer 等待直接阻塞。
讀者正常退訂必須等本機 read 完成；讀者失聯時，中央必須確認該次程序 generation 已退出，
才清掉其 reader 占用。不能只憑 heartbeat 逾時或 PID 重用就搶回仍可能讀取的 slot。
Publisher 正常關閉或程序退出會撤下其歷史；已收到的本機副本保留。
重啟建立新 Publisher instance 與新空間，不回放舊歷史。

同一 Publisher／Subscriber 重疊呼叫資料操作會回 `OperationInProgress`；不同 handle 可並行。
生成訊息、loan 與其欄位仍需呼叫端序列化存取。
對同一 C++ handle 呼叫 `close`、move assignment 或析構時，必須與該 handle 的其他操作外部同步。
控制 RPC 最長等待 10 秒；結果未知時關閉該 Module 控制會話，不自動重送，依既有 Module 政策退出／重啟。
沒有新資料不等待，但 mapping、配置、複製與競爭重試仍有執行成本；不承諾 API 的最差執行時間。

## 限制與實作邊界

`ModuleManager(bus::Limits)` 可設定中央預算：預設 128 topics、每 topic 32 Publishers、
128 Subscribers、1 GiB 活躍共享配置。每 Publisher 最多 65,536 slots；Subscriber 上限可設到 4,096，
Publisher 上限目前不可超過 32（控制封包有界）。Publisher 名稱及 topic 最多 256 UTF-8 bytes。
每個 Client 最多 32 個控制請求等待，Broker 應用請求佇列最多 256；退出回收事件不因該上限被丟棄。

共享預算計算活躍 Ring 的控制區、含對齊的 payload 與每 topic 的 8-byte counter；
不包含 OS page rounding、本機 class／索引／scratch／接收副本，或尚被客戶端持有的退役 mapping。
`PublisherStats::shared_bytes` 是該 Ring 的配置，未含共用 topic counter。
因此它們不是整個系統的 RSS／commit 上限，也不是所有物件都已析構的證明。

共享原子與時間封裝在 OSAL；Bus 不把 Ring 塞進 SharedMemory API。
Windows 使用 Interlocked64；Linux／macOS 使用編譯器原子操作並靜態要求 64-bit always-lock-free。
發布可見性與 slot 協定使用原子 word，整個 `commit`／Bus API 含配置與同步，**不宣稱整體 lock-free**。
目前支援設計範圍為同主機、同版本 framework、64-bit 程序；沒有跨編譯器 ABI／安全沙箱／硬即時保證。
中央崩潰時 POSIX 命名 shared memory 可能殘留，尚未實作跨中央重啟的 orphan 掃描回收。
Windows x64 為實測平台；Linux／macOS 尚待原生建置與故障驗證，未做吞吐量 benchmark。

同步證明、控制協定與測試對照見 [DESIGN.md](DESIGN.md)。

## 可直接操作的範例

Windows Debug 建置後，從儲存庫根目錄執行：

```powershell
.\build\windows-msvc-debug\Vinc.exe
```

依序輸入（新的 Shell，尚無其他 busNode）：

```text
busExample sub demo
busExample pub demo
busNode_1 push 10
busNode last
busNode last
busNode next
busNode_1 loan 20
busNode last
busNode_1 commit
busNode next
busNode_1 stats
exit
```

sub 與 pub 是兩個獨立程序。loan 尚未 commit 時 last 仍得到 10；commit 後 next 得到 20。
空資料／未 commit 再借的錯誤可在互動 Shell 測試，腳本遇到非零命令會停止。

MSVC 繁中工具鏈建議先 `chcp 65001`，再 configure／build；若既有 CMake 編碼偵測錯誤，
以 `cmake --fresh --preset windows-msvc-debug` 重新偵測（`--fresh` 需 CMake 3.24+），
並 `cmake --build --preset windows-msvc-debug --clean-first` 完整重建，避免遺漏標頭相依。

# Shared Memory Message Bus

`vinc::message_bus` 接上 OSAL SharedMemory、Ribosome 生成訊息及獨立程序 Module。
模組在 `on_start()` 之後透過繼承的 `context.advertise<T>()`／`subscribe<T>()` 使用服務；建構子內尚不可使用。
每個 Publisher 擁有獨立 RingBuffer，同一 topic 可有多個 Publisher。大量訊息留在共享記憶體，
Socket 控制通道只交換註冊、退訂與 Publisher 增減資訊。

依序接收透過 Context pump 派送 callback；`get_last()` 提供最新值。沒有額外資料 Socket 通知。

## 命名與驗證

2026-10-03：環形緩衝區型別統一為 `detail::RingBuffer`，共享控制標頭為 `RingBufferHeader`。
檔名為 `ring_buffer.hpp`／`ring_buffer.cpp`，測試為 `ring_buffer_tests.cpp`、
target `message_bus_ring_buffer_tests` 與 CTest `message_bus.ring_buffer`。
成員、常數、引用與文件同步更新，不保留舊名稱別名；共享布局、magic／版本、容量及同步行為維持原值。
此次 Windows Debug 完整建置與相關測試 5／5 通過；Release 獨立測試目標建置與測試 4／4 通過，
兩種組態的 SDK header smoke 均編譯成功。Release 的 Vinc／範例仍在執行中，
因此本次未重建該執行檔或執行 Release Shell 測試；占用限制見下方歷史驗證紀錄。
Linux／macOS 本次未驗證。

2026-10-03：中央管理類別統一為 `detail::BusManager`，檔名為 `bus_manager.hpp`／`bus_manager.cpp`。
ModuleManager 成員與設定常數同步更名；此次只有名稱與引用調整，保留原有執行行為。
Windows Debug 完整建置成功，Message Bus、Module 與 SDK 相關測試 5／5 通過。
Release 的獨立測試目標建置成功，RingBuffer／Module／程序生命週期／SDK 測試 4／4 通過。

2026-10-03：原 `bus` 元件統一更名為 `message_bus`，包含 namespace、標頭、
CMake target、`BaseModule::message_bus()` 與範例 `messageBusExample`；舊名稱不提供相容別名。
控制分類 `Domain::MessageBus` 仍為 3，RingBuffer magic、版本、共享布局及同步行為均維持原值。

本輪 Windows x64／MSVC Debug、Release 完整建置成功，CTest 各 26／26 通過，
包含 `message_bus.ring_buffer`、`message_bus.module`、`message_bus.shell` 與 SDK 程序生命週期測試。
獨立 SDK 使用端及 Module target 建置成功，使用端執行成功。Linux／macOS 本輪未驗證。

### 歷史驗證限制：命名輪次的 Release 程式占用

BusManager 更名驗證時，既有 Release `Vinc.exe` 與其範例模組仍在執行，
連結 `examples/module/vinc_module_example.exe` 時發生 LNK1104（檔案無法開啟）。
當時保留執行中的程序，完成上述獨立測試，未完成該輪 Release 全目標建置與 Shell 測試。
後續可讀性與錯誤遷移輪次已完成 Release 建置及測試，此占用不是目前待辦；Linux／macOS 仍未驗證。

## 元件接線邊界

連結入口為 CMake alias `vinc::message_bus`，實際 library target 為 `vinc_message_bus`。
使用者入口為 `<vinc/message_bus/message_bus.hpp>`。Module Runtime 使用 `detail/runtime.hpp` 接線，
中央 Manager 使用 `detail/bus_manager.hpp` 提交控制要求與確認程序退出；RingBuffer、協定及回收實作仍由 Message Bus 維護。
`include/vinc/message_bus/detail` 的存在不承諾應用可直接操作內部布局。
固定記憶體綁定使用 Message Storage 的指定 detail 契約，不再引用其他元件 src 目錄。

## 程式閱讀順序

2026-10-03：可讀性整理展開控制流程、分開宣告、補上生命週期與布局註解；公開 API、
錯誤碼、例外邊界、控制協定及共享欄位排列維持原有契約。

| 位置 | 建議閱讀重點 |
| --- | --- |
| `include/vinc/message_bus/message_bus.hpp` | Client、Publisher、Subscriber、WriteLoan 的持有與移動行為 |
| `src/message_bus.cpp` | Transport 請求配對與 topology；borrow／commit／push；refresh／read 的保護複製範圍 |
| `src/bus_manager.cpp` | `execute` 分派至 `advertise`、`subscribe`、`close_registration`；worker 序列化註冊與退出回收 |
| `src/ring_buffer.cpp` | `create`／`open` 串接控制區取得、映射、初始化／驗證與 payload 取得；接著看 reserve／publish／protect |
| `include/vinc/message_bus/detail/ring_buffer.hpp` | 固定共享布局、generation token、payload 對齊及版本限制 |
| `include/vinc/message_bus/detail/protocol.hpp` | Descriptor／Attachment 的欄位編碼順序及控制面上限 |

RingBuffer 的 acquisition helper 把成功取得的資源立即交回 RingBuffer 持有；後續失敗由 RingBuffer 析構回收，
只有建立者移除自己成功建立的名稱。Client 的遠端註冊回滾由 `RegistrationCleanup` 維護，
`attach_publisher_resources` 失敗時，先送關閉事件，再釋放本機 core 持有的映射；完整失敗行為見下方重審修正。

預設容量、中央預算、控制等待與重試設定集中到 `<vinc/config.hpp>` 的 `message_bus_*` 常數，
`vinc::message_bus` 透過 CMake PUBLIC 相依提供 `vinc::common`。數值與可讀性整理前相同；
協定／共享布局限制留在元件，測試負載與逾時留在測試。這尚不是動態 Param 設定。

測試依情境分段；RingBuffer 測試另加入控制區／payload 名稱衝突，驗證部分建立失敗的回收與既有名稱保留。

可讀性整理輪次 Windows x64／MSVC Debug、Release 完整建置及 SDK header smoke 編譯通過；
CTest 各 26／26 通過，耗時分別為 46.91 秒與 40.78 秒（不是效能基準）。
整理前後比對確認共享結構欄位型別／順序／陣列長度、四個控制 codec 的運算式順序，
以及 reserve／publish／abandon／candidate／protect 的共享原子操作運算式與順序未變。
此比對與測試不構成所有排程的形式化證明；Linux／macOS 該輪未驗證。

## 三條使用路徑

以下程式位於 `BaseModule` 的 hook；`Frame` 是測試 fixture 的生成訊息，正式專案可替換成自己的 `.msg`。
範例省略錯誤分支的業務處理；每個 Result 都需要檢查。

```cpp
auto registered = context.advertise<vinc::Frame>("frames", {
    .slots = 4, .max_message_bytes = 4 * 1024 * 1024
});
if (!registered) {
    return std::unexpected(registered.error());
}

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

auto registered_sub = context.subscribe<vinc::Frame>("frames", [](std::unique_ptr<vinc::Frame> message) {
    // Owned local message: retain/move it if application processing needs to continue later.
    consume(std::move(message));
});
if (!registered_sub) {
    return std::unexpected(registered_sub.error());
}

auto sub = std::move(*registered_sub);
auto received = sub.get_last(); // 獨立的本地副本，不推進 callback 游標
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
  例如 4 筆 × 4 MiB 有 16 MiB payload 空間，另加 RingBuffer 控制資料。slot 起點補齊 8 bytes；
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
| 訂閱 callback | pump 開始時固定各來源發布上限；依序複製仍保留的資料，成功才推進該來源游標 |
| `get_last()` | 取得目前可見候選中的最新資料，可重複取得同一筆；不改變 callback 進度 |
| 無新資料 | 本次 pump 不派送 callback，正常返回；內部 NoNewData 不變成 pump 失敗 |
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

## Context 批次與遺失統計

公開 Subscriber `next()` 已移除，內部指定接線 `detail/dispatch.hpp` 的 capture/read_batch
供 Context 使用。每輪先擷取本地訂閱集合與每個 Publisher 的 head，僅保存 metadata，
不保護整批 slot 或提前複製整批 payload。新來源／較新發布留到下次 pump。
使用者 callback 的執行緒、所有權與故障政策由 [Module System](../module_system/README.md#context-與執行模式) 維護。

`Subscriber::stats()` 的 `lost_messages`／`overwrite_events` 記錄已確認的覆蓋缺口，
`sources` 提供目前來源 Publisher ID 的分項；來源移除後仍保留總數，不保存無界的來源歷史。
每次發現連續缺口算一個 overwrite event；資料複製期間有新覆蓋可能形成下一事件。
計算使用各 Publisher sequence 與可讀 slot 狀態；借出未 commit 已淘汰的 slot 也計入。
OldestRetained 從序號 1 起計算可見缺口；NewOnly 的初始 floor 不計為遺失。
來源退役／Detach 直接移除，不能確認的剩餘資料不冒充覆蓋損失。get_last 不更新這些進度。
這與 Writer 的讀寫保護碰撞統計分開，不能用 topic count 的差值推算丟失筆數。

## 碰撞、生命週期與並行

讀者先在本機配置接收空間，再保護來源、執行一次 `memcpy`、解除保護，最後驗證本機副本與建構 class。
保護期間沒有使用者 callback。寫入者關閉該 slot 對新讀者的入口，等待既有複製完成。
正常碰撞不設逾時；使用者已選擇這項行為。執行緒暫停／系統排程仍可能造成長時間等待。

`stats()` 提供 `collisions`、完成等待的 `total_wait_ns`／`longest_wait_ns`，
以及尚在等待的 `current_wait_ns`。一次 slot 回收等待計為一次碰撞，不是每次輪詢加一。
統計是近似快照，各欄位非一次原子讀取；時間／次數採飽和累加。

heartbeat 與控制收發由獨立執行緒維護，不受 Message Bus writer 等待直接阻塞。
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

`ModuleManager::create(message_bus::Limits)` 可設定中央預算：預設 128 topics、每 topic 32 Publishers、
128 Subscribers、1 GiB 活躍共享配置。每 Publisher 最多 65,536 slots；Subscriber 上限可設到 4,096，
Publisher 上限目前不可超過 32（控制封包有界）。Publisher 名稱及 topic 最多 256 UTF-8 bytes。
每個 Client 最多 32 個控制請求等待，BusManager 應用請求佇列最多 256；退出回收事件不因該上限被丟棄。

共享預算計算活躍 RingBuffer 的控制區、含對齊的 payload 與每 topic 的 8-byte counter；
不包含 OS page rounding、本機 class／索引／scratch／接收副本，或尚被客戶端持有的退役 mapping。
`PublisherStats::shared_bytes` 是該 RingBuffer 的配置，未含共用 topic counter。
因此它們不是整個系統的 RSS／commit 上限，也不是所有物件都已析構的證明。

共享原子與時間封裝在 OSAL；Message Bus 不把 RingBuffer 塞進 SharedMemory API。
Windows 使用 Interlocked64；Linux／macOS 使用編譯器原子操作並靜態要求 64-bit always-lock-free。
發布可見性與 slot 協定使用原子 word，整個 `commit`／Message Bus API 含配置與同步，**不宣稱整體 lock-free**。
目前支援設計範圍為同主機、同版本 framework、64-bit 程序；沒有跨編譯器 ABI／安全沙箱／硬即時保證。
中央崩潰時 POSIX 命名 shared memory 可能殘留，尚未實作跨中央重啟的 orphan 掃描回收。
Windows x64 為實測平台；Linux／macOS 尚待原生建置與故障驗證，未做吞吐量 benchmark。

同步證明、控制協定與測試對照見 [DESIGN.md](DESIGN.md)。

## 可直接操作的範例

Windows Debug 建置後，從儲存庫根目錄執行：

```powershell
.\build\windows-msvc-debug\Vinc.exe
```

依序輸入（新的 Shell，尚無同名實例；`--name` 由 Message Bus 範例解析）：

```text
messageBusExample --name subscriber sub demo
messageBusExample --name publisher pub demo
publisher push 10
subscriber last
subscriber last
subscriber next
publisher loan 20
subscriber last
publisher commit
subscriber next
publisher stats
exit
```

sub 與 pub 是兩個獨立程序。loan 尚未 commit 時 last 仍得到 10；commit 後 next 得到 20。
空資料／未 commit 再借的錯誤可在互動 Shell 測試，腳本遇到非零命令會停止。

MSVC 繁中工具鏈建議先 `chcp 65001`，再 configure／build；若既有 CMake 編碼偵測錯誤，
以 `cmake --fresh --preset windows-msvc-debug` 重新偵測（`--fresh` 需 CMake 3.24+）。
先前錯誤 catalog 在 clean 後無法重建的限制已由 Common 修正；目前有 build-time
生成規則，見 [Common 重審修正](../common/README.md#2026-10-05-common-重審修正)。

## 統一錯誤介面與跨程序回覆

Client、Publisher、Subscriber、RingBuffer、NamedCounter 及 BusManager／Runtime 接線
皆使用 `vinc::Result<T>`／`vinc::Error`。已移除舊 `message_bus::Error` payload、
Result alias 與 `to_error()`；呼叫端以 `error.is(message_bus::ErrorCode::...)` 判斷，
文字統一由 `vinc::error_to_string(error)` 產生，不再讀取 `.message`。
既有錯誤碼 1–15 保留，舊 `LegacyOperation=6` 退休且不重用；新增操作碼見
`<vinc/message_bus/error.hpp>` 與本元件的靜態 catalog。

- 本層產生的失敗帶有實際操作；同元件內直接傳遞，不重複增加原因層。
- OSAL 共享資源失敗仍分類為 Bus `Io`，透過 `wrap_error()` 保留 OSAL 分類、操作及原生診斷。
- Message Storage 的容量／配置／型別／關閉失敗轉為對應 Bus 分類，其餘維持 `InvalidData`；
  所有分支均保留 Storage 原因。可回傳的控制請求編碼／送出失敗亦保留 Control Channel 原因。
- `NoNewData`／`NoPublishedData`、重疊操作、loan、容量與回收等待語意不變。
  無效 WriteLoan 解參照仍是 contract violation，不改成 Result。
- 現有 bad_alloc／std::exception 邊界及分類保持；數值 Error 不保存 `what()` 動態字串。
  錯誤遷移輪次沒有全面攔截 STL 例外或宣告所有操作不拋例外。
- 通知送出與析構路徑維持既有關閉會話政策，未新增背景故障查詢介面。

Message Bus 控制協定升為 **v2**，操作欄位的高 byte 是版本 2，低 byte 保留原本 1–6
操作含義。新舊操作值互不重疊，舊／未知版本在中央入列或客戶端拓樸處理前拒絕，
不提供舊格式轉接。中央與 Module 必須一起重建；Module bootstrap v3 與外層 Socket frame v1
不變。RingBuffer 共享布局 v1、訊息映像 v2 及資料複製流程不變。

失敗 Response 以數字 Error envelope 傳送目前層、原因筆數、截斷旗標及原生來源／數值；
遠端同層 Bus Error 直接交付呼叫端。未知代碼保留數值，非法封包拒絕並關閉會話。
文字於接收端查詢本地 catalog；不傳 C++ 物件布局。完整欄位與版本規則見 [DESIGN](DESIGN.md)。
`detail/error_protocol.hpp` 僅供本元件 codec、runtime 與測試使用，沒有依賴 Module 私有協定。
ModuleManager／Runtime 收到 Bus 失敗後仍包成 ModuleSystem 錯誤。

### 本輪驗證（2026-10-05）

以下為統一錯誤遷移輪次的紀錄。Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 完整建置（含 SDK header smoke）通過，相關 CTest 各 **17／17** 通過。
範圍包含 Common 錯誤、Message Storage／生成訊息、Control Channel、Message Bus、
Module System 與 SDK consumer；該輪未執行全專案全部 CTest。
最後補入控制請求編碼失敗保留 Control Channel 原因的斷言後，Release 的上述 17 項通過，
Debug 另重跑兩個 Bus error 測試，**2／2** 通過。

新增測試涵蓋真實子程序的原因鏈傳遞、未知代碼、截斷鏈、非法封包與版本拒絕；
詳細對照見 [DESIGN 驗證對照](DESIGN.md#驗證對照)。跨程序 fixture 的錯誤回覆為合成診斷，
不代表已注入 OS 資源耗盡。RingBuffer 的 43 處共享原子呼叫及布局宣告已與遷移前比對，
未改變資料面同步順序。Linux／macOS 未執行；當時保留的配置例外收尾問題已於下方重審修正。

## 2026-10-05 重審修正：註冊回滾與退出回收

本輪重審的三項回收缺口已修正：

- **退出清理不受 Detach 失敗中斷。** `remove_publisher()` 先退役 RingBuffer、扣回活躍預算並移除登記，
  再以 `notify_detached()` 逐一通知訂閱者。通知編碼／傳送例外或 Result 失敗只關閉該接收者會話，
  不阻斷其他通知、後續 topic 的 Reader 清理、peer 移除與空 topic 回收。
  因此已接受的退出工作不再因這條通知失敗路徑提前消耗卻留下死亡 Reader token。
- **註冊回滾涵蓋 handle 交付前的完整階段。** `RegistrationCleanup` 在成功 RPC 返回後立即接管，
  有可信 ID 時送 ClosePublisher／Unsubscribe，退訂亦移除本機 topology；
  尚無可信身分、Descriptor 截斷、零 ID、schema 不符或共享布局／尺寸不合法時關閉會話。
  映射、counter 開啟及本機解碼失敗均受保護；成功交付 core 後才解除 guard。
- **pending 名額由作用域清理。** `PendingCleanup` 在請求入列後建立，所有出口都移除該 pending。
  送出或成功回覆 body 複製若拋例外，結果仍未安全交付，沿用關閉會話政策，拒絕自動重試；
  合法遠端錯誤仍直接交付，正常成功不關閉會話。既有外層 bad_alloc／std::exception 分類保留。

此修正沒有改動公開 API、錯誤碼、控制協定 v2、RingBuffer 布局與原子操作、排序、游標、loan 或無限等待政策。
回滾事件的 send 成功仍只表示進入本機佇列；若送出失敗，沿用關閉會話與確認程序退出後的中央回收。
`drain_until()` 等待已接受的工作完成，不代表所有遠端映射或通知已被接收者釋放／處理。
這不是全面保證所有 STL 配置都不會失敗；退出工作入列及其他既有例外邊界未全面改寫。

### 故障驗證

新增 `message_bus.cleanup`，由同一份 Message Bus 原始碼與真實 Control Channel／OSAL 建立測試。
編譯期 `VINC_MESSAGE_BUS_TESTING` 及 `tests/bus_test_hooks.hpp` 僅供該測試 target；
正式 library 不包含注入入口、狀態或執行期分支。

- 在送出請求、成功 body 複製、Descriptor／Attachment 解碼、Publisher attach 與已完成映射後的 counter 開啟邊界，
  注入 `bad_alloc`；確認錯誤分類、pending 歸零、撤銷事件、本機 topology 移除，以及未知結果關閉會話並拒絕重試。
- 真正的 BusManager 處理撤銷後，在單一 topic／Publisher／Subscriber 名額及精確共享預算下重新註冊成功，
  驗證部分映射後的回滾、預算及 reader 名額可重用。
- 畸形 Advertise 成功回覆涵蓋截斷、schema 不符、零 ID、非法尺寸；確認 Protocol 與會話關閉。
- 同一 owner 在前面 topic 發布、後面 topic 持有 reader token；在 Detach 編碼／送出注入例外，
  另驗已關閉接收者的 send Result 失敗。確認其他接收者仍收到通知、後面 topic 的 token 清零、
  Writer 可重新保留位置，以及 Publisher 名額、共享預算、reader index 可重用。

新測試以 quiescent fixture 模擬已確認退出，沒有假稱真的耗盡 OS 記憶體；
真正子程序崩潰、heartbeat 期間占用及回收由既有 `message_bus.module` 驗證。
所有通訊等待有期限，CTest 另有總期限；不構成所有配置點或所有排程的形式化證明。

### 本輪建置與測試結果

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
相關 Debug／Release target 重新建置通過，建置輸出未見編譯警告。
六項 `message_bus.*`（含新 cleanup）及 `module.process` 各 **7／7** 通過，
Debug 24.61 秒、Release 20.27 秒。已核對兩種組態的注入 define 只屬於 cleanup 測試 target。
未執行全專案其餘測試；Linux／macOS 未建置或執行。

## Message Storage 存取契約接線（2026-10-04）

MessageArray／生成訊息的 checked 操作改用 Result，資料填寫範例及測試已檢查並傳遞結果。
借用內的越界 `[]`、失效 facade 及無效 WriteLoan 解參照屬於 programmer error，透過
`vinc::contract_violation()` 終止當前程序。AccessError catch 轉接已移除；原有標準例外邊界保留。
該輪僅調整存取契約；後續資料面錯誤遷移與控制協定 v2 的現況見上方統一錯誤介面。

### 待驗證疑慮：建置負載下的程序測試逾時

2026-10-04 Array 契約驗證時，Debug 全套 CTest 與 Release 建置並行，
`message_bus.module` 曾回報 ModuleSystem CommandTimeout；同輪其餘 34 個測試通過。
停止並行編譯後，最終 Debug／Release 全套各 35／35 通過，該測試分別耗時 11.79／5.15 秒。
目前只能確認重跑通過，尚未證明逾時由主機負載造成；沒有放寬 timeout 或修改同步行為。
若再次出現，建議另行記錄命令階段、程序排程與等待狀態，區分執行預算不足和生命週期問題。

## Plugin／Host 接線（2026-10-06）

`vinc::message_bus_client` 包含 message_bus.cpp、ring_buffer.cpp，供 Plugin 使用資料面與
Client API；只依賴控制 codec，不連入 Control Service／Session 實作。
`vinc::message_bus` 包含中央 BusManager 與 Host adapter runtime.cpp，並相依 client／control。
原公開 API、共享布局、控制封包、排序與等待政策不變。

Host 建立 Client 時透過 `detail::ControlTransport` 提供 send／close callback，callback 捕捉
Host 所有的 Session。同一 Client 的 Transport 保存配對及拓樸；Plugin 不建立第二個 Client、
第二條 Socket 或接收執行緒。RuntimeAccess 的 Session overload 僅存在於 Host adapter，
ControlTransport overload 為指定框架接線，不是新的使用者 API 或 RuntimeServices 能力。
callbacks 必須保留 Session 所有權，且與 Plugin／Client 的相同建置 ABI 一起使用。
兩個 std::function 在 Client 建立時可能配置，失敗回 ResourceExhausted；不新增 worker，
不為共享資料的 publish／copy 增加傳輸或序列化層。


## Context 批次接線驗證（2026-10-06）

固定上限讀取、覆蓋統計、callback 與移除公開 next 的接線已納入 Windows Debug／Release
回歸；完整案例、結果與平台限制統一記錄於
[Module System Context 驗證](../module_system/README.md#contextpump-實作驗證2026-10-06)。
Bus 原有 ring buffer 共享布局及 writer 等待政策維持。

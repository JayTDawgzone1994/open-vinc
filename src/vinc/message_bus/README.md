<a id="shared-memory-message-bus"></a>

# Shared Memory Message Bus / 共享記憶體訊息匯流排

Message Bus connects OSAL SharedMemory, generated messages, and process Modules. Use inherited Context advertise/subscribe from on_start, not constructors. Each Publisher owns a ring; topics may have several. Bulk payload stays shared, while sockets exchange registration/unsubscribe/topology control.

`vinc::message_bus` 接上 OSAL SharedMemory、Ribosome 生成訊息及獨立程序 Module。
模組在 `on_start()` 之後透過繼承的 `context.advertise<T>()`／`subscribe<T>()` 使用服務；建構子內尚不可使用。
每個 Publisher 擁有獨立 RingBuffer，同一 topic 可有多個 Publisher。大量訊息留在共享記憶體，
Socket 控制通道只交換註冊、退訂與 Publisher 增減資訊。

Sequential delivery runs through Context pump callbacks; get_last reads the latest. No per-message data Socket notifications.

依序接收透過 Context pump 派送 callback；`get_last()` 提供最新值。沒有額外資料 Socket 通知。

Successful publications now signal callback destinations directly through OSAL notifications. Host pumpLoop waits on them with a 10 ms fallback; get_last-only readers create no route. Failure/retry/UI contracts and validation are maintained in [Pump wakeup](../module_system/PUMP_WAKEUP.md).

發布成功後直接透過 OSAL 通知 callback 所屬 Module；pumpLoop 等待通知並保留 10 ms 補查，純 get_last 不建路由。故障／重試／UI 契約及驗證見 [pump 喚醒](../module_system/PUMP_WAKEUP.md)。

The opt-in [benchmark](benchmarks/README.md) measures 1–128 MiB push/loan paths with a4 GiB shared budget per case, without changing defaults/pump settings.

跨程序延遲量測工具見 [Benchmark](benchmarks/README.md)：1～128 MiB、push／loan 兩條路徑，
每組共享配置預算上限 4 GiB。它是 opt-in 工具，不改動框架的預設容量與 pump 設定。

<a id="命名與驗證"></a>

## Names and validation / 命名與驗證

[NAMING](../../../NAMING.md#topic-與-param-名稱分隔符) owns separators: slash allowed, dot forbidden for topics. Prefixes suggest categories, not Publisher identity; publisher_name remains framework-filled.

Topic 與 Param 的分隔符決策由 [NAMING.md](../../../NAMING.md#topic-與-param-名稱分隔符)
維護。Topic 允許 `/`、禁止 `.`；分類前綴是命名建議，不代表發布者身分。
訊息的 `publisher_name()` 保留既有名稱及框架填入行為。

<a id="topic-分隔符驗證2026-10-08"></a>

### Topic separator validation (2026-10-08) / Topic 分隔符驗證（2026-10-08）

Client and central share separator checks in detail/protocol.hpp. Dots return Bus InvalidArgument/Advertise or Subscribe, rejected before local registration/send and before central topic/resources/admission. A bad name does not close healthy Sessions; correct and retry.

`src/message_bus.cpp` 的 Client 與 `src/bus_manager.cpp` 的中央發布／訂閱入口
共用 `detail/protocol.hpp` 的分隔符檢查。含 `.` 的 Topic 回傳 MessageBus
`InvalidArgument`，operation 為 `Advertise` 或 `Subscribe`；Client 在建立註冊所需的
本機狀態與送出要求前拒絕，中央在建立 Topic／共享資源與分配註冊名額前再次驗證。
非法名稱不會關閉正常會話，呼叫端可修正名稱後重新註冊。

Both frames and camera/frames work without required slash count, automatic Module prefix, or separator replacement. Existing empty/NUL/length/encoding behavior remains. Frame layout/version/codes unchanged; old dotted topics must be renamed on both ends without aliases. Benchmark changed only its topic spelling.

`frames` 與 `camera/frames` 均可使用，不強制 `/` 的數量，也不自動補 Module 前綴或
替換分隔符；原有空名稱、NUL、長度及編碼驗證維持原行為。控制封包布局、協定版本
及錯誤碼編號不變；既有含 `.` 的 Topic 必須由發布／訂閱雙方一起改名，沒有相容別名。
Benchmark 已將 `benchmark.payload` 改為 `benchmark/payload`，量測邏輯不變。

Recorded Windows Debug/Release full incremental builds (Release includes benchmark) and **7/7** related suites each (34.19 s/14.75 s). Tests reject client/forged packets before resource consumption, preserve reusable budgets, and verify multiple Publisher names on camera/frames. No full-suite/performance/Linux/macOS run that round.

Windows x64／MSVC Debug、Release 完整增量建置成功，Release 含 Benchmark target。
兩種組態的 Message Bus 五套測試、`module.context` 與 `message_bus.shell` 各 **7/7**
通過（34.19／14.75 秒，非效能量測）。新增案例確認 Client 不送出非法要求、中央拒絕
繞過 Client 的真實封包、錯誤操作碼及拒絕後名額／共享預算仍可使用；跨程序案例以
`camera/frames` 驗證多發布者，並比對兩個 Module 的 `publisher_name()`。
本輪未執行全專案測試、Benchmark 效能量測或 Linux／macOS 驗證。

Param and Module separator rules are now implemented: parameters use module.param and Module names forbid dots/slashes. See [Param](../param/README.md) and NAMING; this is no longer pending Param design.

Param 與 Module 名稱限制已實作：參數採 module.param，Module 名稱禁止點與斜線。見 [Param](../param/README.md) 與 NAMING，不再列為未完成設計。

<a id="歷史命名與驗證紀錄"></a>

### Historical naming evidence / 歷史命名與驗證紀錄

Recorded 2026-10-03: renamed RingBuffer/Header/files/test target with no aliases, preserving layout/magic/capacity/synchronization. Debug full build/related **5/5**, Release standalone **4/4**, both SDK headers passed. Running Release central/examples prevented that round's executable/Shell rebuild; later evidence resolved the restriction. No Linux/macOS then.

2026-10-03：環形緩衝區型別統一為 `detail::RingBuffer`，共享控制標頭為 `RingBufferHeader`。
檔名為 `ring_buffer.hpp`／`ring_buffer.cpp`，測試為 `ring_buffer_tests.cpp`、
target `message_bus_ring_buffer_tests` 與 CTest `message_bus.ring_buffer`。
成員、常數、引用與文件同步更新，不保留舊名稱別名；共享布局、magic／版本、容量及同步行為維持原值。
此次 Windows Debug 完整建置與相關測試 5／5 通過；Release 獨立測試目標建置與測試 4／4 通過，
兩種組態的 SDK header smoke 均編譯成功。Release 的 Vinc／範例仍在執行中，
因此本次未重建該執行檔或執行 Release Shell 測試；占用限制見下方歷史驗證紀錄。
Linux／macOS 本次未驗證。

Same day BusManager/files/Manager members/config names changed only spelling/references. Debug full build/related **5/5**, Release standalone ring/Module/process/SDK **4/4** passed.

2026-10-03：中央管理類別統一為 `detail::BusManager`，檔名為 `bus_manager.hpp`／`bus_manager.cpp`。
ModuleManager 成員與設定常數同步更名；此次只有名稱與引用調整，保留原有執行行為。
Windows Debug 完整建置成功，Message Bus、Module 與 SDK 相關測試 5／5 通過。
Release 的獨立測試目標建置成功，RingBuffer／Module／程序生命週期／SDK 測試 4／4 通過。

Historical bus→message_bus migration renamed namespace/headers/targets/the then-existing BaseModule accessor/example without aliases. Current APIs use Context instead. Domain 3 and then-current ring metadata/synchronization were preserved.

2026-10-03：原 `bus` 元件統一更名為 `message_bus`，包含 namespace、標頭、
CMake target、`BaseModule::message_bus()` 與範例 `messageBusExample`；舊名稱不提供相容別名。
控制分類 `Domain::MessageBus` 仍為 3，RingBuffer magic、版本、共享布局及同步行為均維持原值。
這段記錄當時命名；目前公開入口為繼承的 Context，舊 accessor 不再使用。

That round Windows full Debug/Release builds and **26/26** CTests each passed, including ring/Module/Shell/SDK. Standalone SDK/Module built and consumer ran; no Linux/macOS.

本輪 Windows x64／MSVC Debug、Release 完整建置成功，CTest 各 26／26 通過，
包含 `message_bus.ring_buffer`、`message_bus.module`、`message_bus.shell` 與 SDK 程序生命週期測試。
獨立 SDK 使用端及 Module target 建置成功，使用端執行成功。Linux／macOS 本輪未驗證。

<a id="歷史驗證限制命名輪次的-release-程式占用"></a>

### Resolved historical Release executable occupancy / 歷史驗證限制：命名輪次的 Release 程式占用

The BusManager rename encountered LNK1104 while running Release Vinc/examples held old executable files. Those processes were left intact and standalone tests completed. Later readability/error rounds rebuilt Release/tested Shell; this is no current TODO. It provides no Linux/macOS evidence.

BusManager 更名驗證時，既有 Release `Vinc.exe` 與其範例模組仍在執行，
連結 `examples/module/vinc_module_example.exe` 時發生 LNK1104（檔案無法開啟）。
當時保留執行中的程序，完成上述獨立測試，未完成該輪 Release 全目標建置與 Shell 測試。
後續可讀性與錯誤遷移輪次已完成 Release 建置及測試，此占用不是目前待辦；Linux／macOS 仍未驗證。

<a id="元件接線邊界"></a>

## Integration boundaries / 元件接線邊界

Public header message_bus.hpp/target vinc::message_bus (vinc_message_bus). Runtime uses detail/runtime.hpp, Manager detail/bus_manager.hpp for requests/confirmed exit; Bus owns ring/protocol/recovery. Visible detail is not a user layout API. Fixed binding uses designated Storage detail, not foreign src includes.

連結入口為 CMake alias `vinc::message_bus`，實際 library target 為 `vinc_message_bus`。
使用者入口為 `<vinc/message_bus/message_bus.hpp>`。Module Runtime 使用 `detail/runtime.hpp` 接線，
中央 Manager 使用 `detail/bus_manager.hpp` 提交控制要求與確認程序退出；RingBuffer、協定及回收實作仍由 Message Bus 維護。
`include/vinc/message_bus/detail` 的存在不承諾應用可直接操作內部布局。
固定記憶體綁定使用 Message Storage 的指定 detail 契約，不再引用其他元件 src 目錄。

<a id="程式閱讀順序"></a>

## Implementation reading order / 程式閱讀順序

The2026-10-03 readability pass expanded flow/declarations/lifetime comments, retaining API/errors/exception/protocol/layout contracts.

2026-10-03：可讀性整理展開控制流程、分開宣告、補上生命週期與布局註解；公開 API、
錯誤碼、例外邊界、控制協定及共享欄位排列維持原有契約。

| Location | Focus |
| --- | --- |
| message_bus.hpp | Client/Pub/Sub/WriteLoan ownership/move |
| message_bus.cpp | Request pairing/topology; loan/commit/push; protected copies |
| bus_manager.cpp | Dispatch/register/close, serialized worker/recovery |
| ring_buffer.cpp | Control/payload acquire/map/init/validate, reserve/publish/protect |
| detail/ring_buffer.hpp | Layout/generation/alignment/versions |
| detail/protocol.hpp | Descriptor/Attachment encoding and bounds |

| 位置 | 建議閱讀重點 |
| --- | --- |
| `include/vinc/message_bus/message_bus.hpp` | Client、Publisher、Subscriber、WriteLoan 的持有與移動行為 |
| `src/message_bus.cpp` | Transport 請求配對與 topology；borrow／commit／push；refresh／read 的保護複製範圍 |
| `src/bus_manager.cpp` | `execute` 分派至 `advertise`、`subscribe`、`close_registration`；worker 序列化註冊與退出回收 |
| `src/ring_buffer.cpp` | `create`／`open` 串接控制區取得、映射、初始化／驗證與 payload 取得；接著看 reserve／publish／protect |
| `include/vinc/message_bus/detail/ring_buffer.hpp` | 固定共享布局、generation token、payload 對齊及版本限制 |
| `include/vinc/message_bus/detail/protocol.hpp` | Descriptor／Attachment 的欄位編碼順序及控制面上限 |

Acquisition helpers immediately transfer resources to RingBuffer; later failure uses its destructor, with only creators unlinking their own names. RegistrationCleanup handles remote rollback; failed Publisher attachment sends closure before releasing core mappings. Detailed failure policy is below.

RingBuffer 的 acquisition helper 把成功取得的資源立即交回 RingBuffer 持有；後續失敗由 RingBuffer 析構回收，
只有建立者移除自己成功建立的名稱。Client 的遠端註冊回滾由 `RegistrationCleanup` 維護，
`attach_publisher_resources` 失敗時，先送關閉事件，再釋放本機 core 持有的映射；完整失敗行為見下方重審修正。

Compile-time message_bus_* config centralizes capacities/budgets/control waits/retries and publicly propagates Common. Values were preserved during readability work; protocol/layout/test constants remain owned locally. This is not runtime Param rebinding.

預設容量、中央預算、控制等待與重試設定集中到 `<vinc/config.hpp>` 的 `message_bus_*` 常數，
`vinc::message_bus` 透過 CMake PUBLIC 相依提供 `vinc::common`。數值與可讀性整理前相同；
協定／共享布局限制留在元件，測試負載與逾時留在測試。這尚不是動態 Param 設定。

Tests are scenario-grouped; conflicting control/data names check partial-create rollback without deleting existing resources.

測試依情境分段；RingBuffer 測試另加入控制區／payload 名稱衝突，驗證部分建立失敗的回收與既有名稱保留。

Historical Windows full builds/headers and **26/26** each passed (46.91 s/40.78 s, not benchmarks). Layout/codec expressions and reserve/publish/abandon/candidate/protect atomics were compared unchanged. Not a formal proof for all schedules; no Linux/macOS that round.

可讀性整理輪次 Windows x64／MSVC Debug、Release 完整建置及 SDK header smoke 編譯通過；
CTest 各 26／26 通過，耗時分別為 46.91 秒與 40.78 秒（不是效能基準）。
整理前後比對確認共享結構欄位型別／順序／陣列長度、四個控制 codec 的運算式順序，
以及 reserve／publish／abandon／candidate／protect 的共享原子操作運算式與順序未變。
此比對與測試不構成所有排程的形式化證明；Linux／macOS 該輪未驗證。

<a id="三條使用路徑"></a>

## Three usage paths / 三條使用路徑

This fragment belongs inside a BaseModule hook. Frame is a generated test fixture; substitute your own .msg. Results must be checked; application-specific recovery is omitted.

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
if (!sent) {
    return std::unexpected(sent.error());
}

// 借到共享 slot，填寫後提交；commit 不複製整筆 payload。
auto borrowed = pub.get_next();
if (!borrowed) {
    return std::unexpected(borrowed.error());
}

{
    auto& loan = *borrowed;
    loan->data().sequence = 43;
    auto committed = pub.commit(loan);
    if (!committed) {
        return std::unexpected(committed.error());
    }
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

Real Modules normally retain Pub/Sub members from on_start. Linking vinc_messages supplies generated-header dependencies.

正式模組通常把 Publisher／Subscriber 存成成員，於 `on_start` 建立。
生成訊息 header 的 CMake 相依由 `target_link_libraries(target PRIVATE vinc_messages)` 提供。

<a id="容量與借用契約"></a>

## Capacity and loans / 容量與借用契約

- Default 2 slots; 1 promotes to 2,0 rejects, with no extra N+1 slot.
- max_message_bytes counts full image/header/descriptors/indices/data/reserves. Four 4 MiB slots reserve 16 MiB payload plus controls; 8-byte stride alignment never increases writable capacity.
- Push preserves local source, checks compact capacity before history eviction, and removes unused reserve on success. Other allocation failures need not preserve every historical sample, but incomplete new data never publishes.
- get_next retires the oldest reusable slot while preserving the previous latest. Loan resize/reserve immediately obey limits and preserve pre-operation fields on failure.
- One outstanding loan/Publisher. Another loan or push returns OutstandingLoan. Dropping it does not release state; close the Publisher.
- Move-only loan commits only to its issuer and is consumed/revoked on success. Copied message/array values are independent local data, not committable substitutes.
- Commit validates/allocates local metadata, then obtains times/count and publishes atomically. Zero-copy commit avoids full payload copying, not resize/layout/validation costs.

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

<a id="讀取排序與識別"></a>

## Reading, ordering, identity / 讀取、排序與識別

| Operation | Behavior |
| --- | --- |
| Callback | Fix source heads at pump start; copy retained samples in order, advancing only after success |
| get_last | Latest observed committed candidate, repeatable, without callback-progress change |
| No new data | No callback, normal pump success; internal NoNewData is not failure |
| No Publisher/readable data | Immediate NoPublishedData |
| Lagging Reader | Skip overwritten history, resume oldest retained per source |

| 操作 | 行為 |
| --- | --- |
| 訂閱 callback | pump 開始時固定各來源發布上限；依序複製仍保留的資料，成功才推進該來源游標 |
| `get_last()` | 取得目前可見候選中的最新資料，可重複取得同一筆；不改變 callback 進度 |
| 無新資料 | 本次 pump 不派送 callback，正常返回；內部 NoNewData 不變成 pump 失敗 |
| 尚無 Publisher／任何可讀資料 | `get_last()` 立即回 `NoPublishedData` |
| 讀者落後 | 被覆蓋的歷史跳過，從該來源仍保留的最舊資料繼續 |

Default OldestRetained. NewOnly skips each Publisher's pre-registration data; future Publishers start at their first sample. get_last ignores this initial floor.

預設新訂閱從 `StartPosition::OldestRetained` 開始。
`SubscriberOptions{.start = StartPosition::NewOnly}` 略過註冊時各 Publisher 已發布的資料；
之後新增 Publisher 從其第一筆開始。`get_last` 不受 NewOnly 起點限制。

Cross-Publisher ordering compares same-host monotonic times, breaking ties by earlier registration; each Publisher retains its own order. Only observed committed candidates are compared, without atomic snapshots/global strict FIFO. A Publisher paused after clock sampling may reveal an older timestamp later; delivered data is not reordered backward.

跨 Publisher 比較 OS 單調時間戳，相同時間取較早註冊的 Publisher；同一 Publisher 維持自身發布順序。
只比較這次掃描所觀察到的已提交候選，不提供所有 Publisher 的原子快照或全域嚴格 FIFO。
取得時間後被 OS 暫停的 Publisher，可能稍後才讓較舊時間戳可見；不等待或回頭重排已交付訊息。

Calendar getter returns unsigned 64 Unix nanoseconds; monotonic getter unsigned 64 same-host nanoseconds. Units do not guarantee precision, and wall-clock adjustments do not change sorting. Framework owns six read-only metadata getters; see [dual time](#雙時間發布-metadata2026-10-13). Shared topic count can have gaps and is not ordering; it resets after topic retirement. Publisher IDs do not repeat during Manager lifetime but are not persistent across central restart. Slot generations separately prevent ABA; exhaustion rejects without wrap.

`get_message_timestamp()` 是 `uint64_t` Unix nanoseconds，供日期追查；
`get_message_monotonic_timestamp()` 是 `uint64_t` 同主機單調 nanoseconds，供 Bus 排序。
兩者均不承諾 nanosecond 實際精度，UTC 校時不影響單調排序。
框架填入 `topic_name()`、`publisher_name()`、`publisher_instance()`、
上述兩個時間與 `get_topic_count()`；使用者沒有 metadata setter。
完整發布時機及相容性見[雙時間發布 metadata](#雙時間發布-metadata2026-10-13)。
topic count 在同一 topic 存活期間由各 Publisher 共用 atomic 計數器配置，可能因失敗有缺號，
不是排序依據。topic 無訂閱也無 Publisher 後釋放，重建時 count 重新開始；
Publisher instance ID 在同一 ModuleManager 生命週期不重用，不能跨中央重啟當持久識別。
slot 的 generation 另行防止重用造成 ABA；計數／generation 耗盡時拒絕，不循環回零。

<a id="context-批次與遺失統計"></a>

## Context batches and loss statistics / Context 批次與遺失統計

Public next is removed. Designated detail/dispatch capture/read_batch fixes local subscription sets/source heads using metadata only, without protecting/copying entire batches. New sources/publications wait until later pumps. [Module System](../module_system/README.md#context-與執行模式) owns callback threading/ownership/faults.

公開 Subscriber `next()` 已移除，內部指定接線 `detail/dispatch.hpp` 的 capture/read_batch
供 Context 使用。每輪先擷取本地訂閱集合與每個 Publisher 的 head，僅保存 metadata，
不保護整批 slot 或提前複製整批 payload。新來源／較新發布留到下次 pump。
使用者 callback 的執行緒、所有權與故障政策由 [Module System](../module_system/README.md#context-與執行模式) 維護。

Stats retain confirmed lost_messages/overwrite_events with current-source details and cumulative totals after source removal, not unbounded history. One continuous gap is an event; copying may discover later gaps. Calculations use source sequences/readable slots, including slots retired by uncommitted loans. OldestRetained counts gaps from 1; NewOnly excludes its initial floor. Detach retires unobservable remaining data without inventing loss; get_last changes no cursor. Writer collisions are separate; topic-count differences are not loss counts.

`Subscriber::stats()` 的 `lost_messages`／`overwrite_events` 記錄已確認的覆蓋缺口，
`sources` 提供目前來源 Publisher ID 的分項；來源移除後仍保留總數，不保存無界的來源歷史。
每次發現連續缺口算一個 overwrite event；資料複製期間有新覆蓋可能形成下一事件。
計算使用各 Publisher sequence 與可讀 slot 狀態；借出未 commit 已淘汰的 slot 也計入。
OldestRetained 從序號 1 起計算可見缺口；NewOnly 的初始 floor 不計為遺失。
來源退役／Detach 直接移除，不能確認的剩餘資料不冒充覆蓋損失。get_last 不更新這些進度。
這與 Writer 的讀寫保護碰撞統計分開，不能用 topic count 的差值推算丟失筆數。

<a id="碰撞生命週期與並行"></a>

## Contention, lifetime, concurrency / 碰撞、生命週期與並行

Reader allocates local storage first, protects one source, performs memcpy, releases, then validates/builds locally, with no user callback under protection. Writer closes admission to that slot and waits for existing copies. Approved contention waits have no timeout; scheduling/thread suspension can delay them indefinitely.

讀者先在本機配置接收空間，再保護來源、執行一次 `memcpy`、解除保護，最後驗證本機副本與建構 class。
保護期間沒有使用者 callback。寫入者關閉該 slot 對新讀者的入口，等待既有複製完成。
正常碰撞不設逾時；使用者已選擇這項行為。執行緒暫停／系統排程仍可能造成長時間等待。

Publisher stats expose collision count, completed total/longest wait, and current ongoing wait. One reservation wait counts once, not each poll. Values saturate and form approximate, nontransactional snapshots.

`stats()` 提供 `collisions`、完成等待的 `total_wait_ns`／`longest_wait_ns`，
以及尚在等待的 `current_wait_ns`。一次 slot 回收等待計為一次碰撞，不是每次輪詢加一。
統計是近似快照，各欄位非一次原子讀取；時間／次數採飽和累加。

Separate control/heartbeat threads are not directly blocked by Writer waits. Unsubscribe waits for local reads; dead Readers reclaim only after central confirms that process generation exited, not heartbeat timeout/reused PID. Publisher close/exit retires history but local copies survive. Restart uses new IDs/storage without replay.

heartbeat 與控制收發由獨立執行緒維護，不受 Message Bus writer 等待直接阻塞。
讀者正常退訂必須等本機 read 完成；讀者失聯時，中央必須確認該次程序 generation 已退出，
才清掉其 reader 占用。不能只憑 heartbeat 逾時或 PID 重用就搶回仍可能讀取的 slot。
Publisher 正常關閉或程序退出會撤下其歷史；已收到的本機副本保留。
重啟建立新 Publisher instance 與新空間，不回放舊歷史。

Overlapping data operations on one Pub/Sub return OperationInProgress; different handles can run concurrently. Serialize messages/loans/fields and same-handle close/move/destruction. Control requests wait up to 10 s; uncertain outcomes close the Module control Session, with no resend, following fault/restart policy. No-data reads do not wait for publication, but mapping/allocation/copy/contention still cost time without worst-case API guarantees.

同一 Publisher／Subscriber 重疊呼叫資料操作會回 `OperationInProgress`；不同 handle 可並行。
生成訊息、loan 與其欄位仍需呼叫端序列化存取。
對同一 C++ handle 呼叫 `close`、move assignment 或析構時，必須與該 handle 的其他操作外部同步。
控制 RPC 最長等待 10 秒；結果未知時關閉該 Module 控制會話，不自動重送，依既有 Module 政策退出／重啟。
沒有新資料不等待，但 mapping、配置、複製與競爭重試仍有執行成本；不承諾 API 的最差執行時間。

<a id="限制與實作邊界"></a>

## Limits and implementation scope / 限制與實作邊界

Manager Bus limits default 128 topics,32 Publishers/topic,128 Subscribers, 1 GiB active shared bytes. Maximum 65536 slots,4096 Subscribers,32 Publishers due to bounded descriptors. Names/topics<=256 UTF-8 bytes. Each Client has 32 pending controls, manager 256 application requests; accepted exit recovery is not dropped by that cap.

框架內部 `ModuleManager::create(limits, host_executable)` 可設定中央預算，Host 路徑由框架提供：
預設 128 topics、每 topic 32 Publishers、
128 Subscribers、1 GiB 活躍共享配置。每 Publisher 最多 65,536 slots；Subscriber 上限可設到 4,096，
Publisher 上限目前不可超過 32（控制封包有界）。Publisher 名稱及 topic 最多 256 UTF-8 bytes。
每個 Client 最多 32 個控制請求等待，BusManager 應用請求佇列最多 256；退出回收事件不因該上限被丟棄。

Shared accounting includes active ring controls/aligned payload and 8-byte/topic counter, not OS rounding/local classes/indices/scratch/copies/retired mappings still held by clients. Publisher shared_bytes excludes the common counter. This is neither RSS/commit limit nor proof all objects were destroyed.

共享預算計算活躍 RingBuffer 的控制區、含對齊的 payload 與每 topic 的 8-byte counter；
不包含 OS page rounding、本機 class／索引／scratch／接收副本，或尚被客戶端持有的退役 mapping。
`PublisherStats::shared_bytes` 是該 RingBuffer 的配置，未含共用 topic counter。
因此它們不是整個系統的 RSS／commit 上限，也不是所有物件都已析構的證明。

OSAL owns atomics/time; rings do not belong in SharedMemory. Windows uses Interlocked64, POSIX compile-time 64-bit always-lock-free atomics. Whole APIs include allocation/synchronization and **are not claimed lock-free**. Scope is same-host/version 64-bit, not cross-compiler ABI/security/hard real-time. Production Instance records names before creation and cleans recorded old epochs at startup; standalone random-name tests/unrecorded legacy orphans have no such guarantee. Platform evidence is recorded with source revisions and suite scope in the [platform reports](../../../README.md#validation-and-limits). [Paced benchmarks](benchmarks/README.md) are not saturation throughput.

共享原子與時間封裝在 OSAL；Message Bus 不把 RingBuffer 塞進 SharedMemory API。
Windows 使用 Interlocked64；Linux／macOS 使用編譯器原子操作並靜態要求 64-bit always-lock-free。
發布可見性與 slot 協定使用原子 word，整個 `commit`／Message Bus API 含配置與同步，**不宣稱整體 lock-free**。
目前支援設計範圍為同主機、同版本 framework、64-bit 程序；沒有跨編譯器 ABI／安全沙箱／硬即時保證。
正式 Instance 接線已有建立前名稱記錄及下次啟動清理；未注入服務的獨立測試與未留下紀錄的舊 orphan 不在保證內，見下方名稱服務章節。
各平台的實測版本與套件範圍見[平台驗證報告](../../../README.md#validation-and-limits)；原生故障注入仍依各項案例紀錄。
有節奏的延遲／資料率量測見 [Benchmark](benchmarks/README.md)；尚未測量飽和吞吐量。

Synchronization reasoning/protocol/tests: [DESIGN](DESIGN.md).

同步證明、控制協定與測試對照見 [DESIGN.md](DESIGN.md)。

<a id="可直接操作的範例"></a>

## Interactive example / 可直接操作的範例

The current example uses callback delivery and last; the removed next command is not restored. The sequence below matches examples/message_bus/message_bus_example.cpp.

目前範例使用 callback 與 last；已移除的 next 命令不恢復。以下操作已按 examples/message_bus/message_bus_example.cpp 核對。

After a Windows Debug build, run from repository root:

Windows Debug 建置後，從儲存庫根目錄執行：

```powershell
.\build\windows-msvc-debug\Vinc.exe
```

In a fresh Shell with unique names (--name is parsed by this example):

依序輸入（新的 Shell，尚無同名實例；`--name` 由 Message Bus 範例解析）：

```text
messageBusExample --name subscriber sub demo
messageBusExample --name publisher pub demo
publisher push 10
subscriber last
subscriber last
subscriber last
publisher loan 20
subscriber last
publisher commit
subscriber last
publisher stats
exit
```

Two independent processes participate. After push 10, callback eventually runs and repeated last reads 10. An uncommitted loan 20 leaves last at 10; after commit the next observed last is 20. Allow pump scheduling before querying. Missing-data/outstanding-loan errors are interactive tests; Tcl stops on nonzero commands.

兩個獨立程序參與。push 10 後 callback 會由 pump 派送，重複 last 可讀同筆。loan 20 尚未 commit 時 last 仍為 10；commit 後觀察到的新 last 為 20。請留時間讓 pump 執行。無資料／重複 loan 錯誤可在互動 Shell 測試；Tcl 遇非零會停止。

Use a UTF-8 MSVC developer environment (for example chcp65001 before configure/build). The localized MSVC/Ninja dependency-prefix fix is now in [SDK](../sdk/README.md#msvcninja-增量建置修復2026-10-06); affected old objects need reconfigure plus one clean rebuild. Common also fixed catalog clean/regeneration; see [review notes](../common/README.md#2026-10-05-common-重審修正).

MSVC 請使用 UTF-8 開發環境，例如 configure／build 前 chcp 65001。本地化 Ninja 標頭相依修復見 [SDK](../sdk/README.md#msvcninja-增量建置修復2026-10-06)；既有受影響產物需重新配置並 clean rebuild 一次。Catalog clean 重建亦已修復，見 [Common](../common/README.md#2026-10-05-common-重審修正)。

<a id="統一錯誤介面與跨程序回覆"></a>

## Unified errors and cross-process responses / 統一錯誤介面與跨程序回覆

Client/Pub/Sub/Ring/Counter/Manager/Runtime all use Common Result/Error. Legacy payload/alias/to_error/message text fields are removed; use typed is and common error_to_string. Codes 1–15 remain, LegacyOperation 6 is retired, and later operations belong to error.hpp/static catalog.

Client、Publisher、Subscriber、RingBuffer、NamedCounter 及 BusManager／Runtime 接線
皆使用 `vinc::Result<T>`／`vinc::Error`。已移除舊 `message_bus::Error` payload、
Result alias 與 `to_error()`；呼叫端以 `error.is(message_bus::ErrorCode::...)` 判斷，
文字統一由 `vinc::error_to_string(error)` 產生，不再讀取 `.message`。
既有錯誤碼 1–15 保留，舊 `LegacyOperation=6` 退休且不重用；新增操作碼見
`<vinc/message_bus/error.hpp>` 與本元件的靜態 catalog。

- Local failures carry the failing operation, without redundant same-component wrapping.
- OSAL shared failures map to Bus Io with retained native/cause data.
- Storage capacity/allocation/type/closed failures map to appropriate Bus classes, others InvalidData, all retaining causes. Control encoding/send Results retain Control causes.
- NoNewData/NoPublishedData/overlap/loan/capacity/recovery semantics remain. Invalid WriteLoan access is fatal, not Result.
- Existing bad_alloc/std::exception conversion remains; numeric Error owns no what() string. Migration was not wholesale exception catching/noexcept.
- Notification/destructor failures retain Session-close policy, without a new background diagnostic API.

- 本層產生的失敗帶有實際操作；同元件內直接傳遞，不重複增加原因層。
- OSAL 共享資源失敗仍分類為 Bus `Io`，透過 `wrap_error()` 保留 OSAL 分類、操作及原生診斷。
- Message Storage 的容量／配置／型別／關閉失敗轉為對應 Bus 分類，其餘維持 `InvalidData`；
  所有分支均保留 Storage 原因。可回傳的控制請求編碼／送出失敗亦保留 Control Channel 原因。
- `NoNewData`／`NoPublishedData`、重疊操作、loan、容量與回收等待語意不變。
  無效 WriteLoan 解參照仍是 contract violation，不改成 Result。
- 現有 bad_alloc／std::exception 邊界及分類保持；數值 Error 不保存 `what()` 動態字串。
  錯誤遷移輪次沒有全面攔截 STL 例外或宣告所有操作不拋例外。
- 通知送出與析構路徑維持既有關閉會話政策，未新增背景故障查詢介面。

Current control protocol is Bus v3: high operation byte 3, original operations 1–6 in the low byte, plus list/inspect/connection and callback-notification operations. Old/unknown versions reject before queue/topology, without adapters. Frame stays v1; current bootstrap is v16, ring layout v3, Storage image v2. Historical migration preserved its then-current bootstrap/ring versions, not those values forever. Rebuild all connected participants.

目前 Bus 控制協定為 v3，高 byte 為 3，低 byte 保留原 1–6，另含清單／診斷／連線及 callback 通知操作。舊／未知版本在入列或拓樸處理前拒絕，不相容轉接。外層 frame v1；目前 bootstrap v16、RingBuffer v3、Storage 映像 v2。遷移當時保留舊 bootstrap／ring 不表示它們永不升版；相連程式須一起重建。

Failure responses explicitly encode current error/causes/count/truncation/native fields; same-layer remote Bus errors pass through. Unknown codes stay numeric; malformed packets close Sessions. Text resolves in local catalogs, never object-layout transport. [DESIGN](DESIGN.md) owns bytes. detail/error_protocol is only Bus codec/runtime/test integration, independent of Module-private protocol; Module wraps Bus failures at its boundary.

失敗 Response 以數字 Error envelope 傳送目前層、原因筆數、截斷旗標及原生來源／數值；
遠端同層 Bus Error 直接交付呼叫端。未知代碼保留數值，非法封包拒絕並關閉會話。
文字於接收端查詢本地 catalog；不傳 C++ 物件布局。完整欄位與版本規則見 [DESIGN](DESIGN.md)。
`detail/error_protocol.hpp` 僅供本元件 codec、runtime 與測試使用，沒有依賴 Module 私有協定。
ModuleManager／Runtime 收到 Bus 失敗後仍包成 ModuleSystem 錯誤。

<a id="本輪驗證2026-10-05"></a>

### Error migration validation (2026-10-05) / 本輪驗證（2026-10-05）

Windows x64/MSVC 19.51.36257/SDK 10.0.26100.0/CMake 4.3.2/Ninja 1.13.2 full Debug/Release builds/headers and **17/17** related tests passed, not all-project. After final Control-cause assertions, Release 17 passed and Debug reran two Bus error tests **2/2**.

以下為統一錯誤遷移輪次的紀錄。Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 完整建置（含 SDK header smoke）通過，相關 CTest 各 **17／17** 通過。
範圍包含 Common 錯誤、Message Storage／生成訊息、Control Channel、Message Bus、
Module System 與 SDK consumer；該輪未執行全專案全部 CTest。
最後補入控制請求編碼失敗保留 Control Channel 原因的斷言後，Release 的上述 17 項通過，
Debug 另重跑兩個 Bus error 測試，**2／2** 通過。

Real-child cause/unknown/truncation/malformed/version tests use synthetic diagnostics, not OS exhaustion. See [test mapping](DESIGN.md#驗證對照).43 shared atomic calls/layout declarations matched pre-migration order. No Linux/macOS; then-pending allocation cleanup was fixed below.

新增測試涵蓋真實子程序的原因鏈傳遞、未知代碼、截斷鏈、非法封包與版本拒絕；
詳細對照見 [DESIGN 驗證對照](DESIGN.md#驗證對照)。跨程序 fixture 的錯誤回覆為合成診斷，
不代表已注入 OS 資源耗盡。RingBuffer 的 43 處共享原子呼叫及布局宣告已與遷移前比對，
未改變資料面同步順序。Linux／macOS 未執行；當時保留的配置例外收尾問題已於下方重審修正。

<a id="2026-10-05-重審修正註冊回滾與退出回收"></a>

## Registration rollback and exit recovery fixes (2026-10-05) / 2026-10-05 重審修正：註冊回滾與退出回收

Three recovery gaps were fixed:

本輪重審的三項回收缺口已修正：

- Detach failure no longer aborts exit cleanup: retire ring/reclaim active budget/remove registration before notifying each subscriber. Encoding/send exceptions/Results close only that receiver and do not interrupt remaining notifications/topic readers/peers/pruning, leaving no consumed-but-unfinished exit job on this path.
- RegistrationCleanup takes ownership immediately after successful response. Trusted IDs send ClosePublisher/Unsubscribe and remove local topology; untrusted/truncated/zero/mismatched/invalid descriptors close the Session. Mapping/counter/decode failures stay covered until core delivery.
- PendingCleanup removes pending entries on every exit. Send/body-copy exceptions before safe response delivery close the Session/no retry; valid remote errors return normally, success does not close. Existing outer exception classification remains.

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

These fixes preserved APIs/codes/v2/ring atomics/order/cursors/loans/infinite waits. Rollback-send success means local acceptance; failed send falls back to close/confirmed-exit recovery. drain_until means accepted jobs finished, not all remote mappings/notifications released. Other allocation/admission boundaries were not universally rewritten.

此修正沒有改動公開 API、錯誤碼、控制協定 v2、RingBuffer 布局與原子操作、排序、游標、loan 或無限等待政策。
回滾事件的 send 成功仍只表示進入本機佇列；若送出失敗，沿用關閉會話與確認程序退出後的中央回收。
`drain_until()` 等待已接受的工作完成，不代表所有遠端映射或通知已被接收者釋放／處理。
這不是全面保證所有 STL 配置都不會失敗；退出工作入列及其他既有例外邊界未全面改寫。

<a id="故障驗證"></a>

### Fault validation / 故障驗證

message_bus.cleanup compiles the same Bus sources with real Control/OSAL. Test-only VINC_MESSAGE_BUS_TESTING/hooks are absent from production state/API/branches.

新增 `message_bus.cleanup`，由同一份 Message Bus 原始碼與真實 Control Channel／OSAL 建立測試。
編譯期 `VINC_MESSAGE_BUS_TESTING` 及 `tests/bus_test_hooks.hpp` 僅供該測試 target；
正式 library 不包含注入入口、狀態或執行期分支。

- Inject bad_alloc at request send, success-body copy, descriptor/attachment decode, Publisher attach, and post-map counter open. Check codes, pending 0, rollback events/topology, uncertain-result closure/no retries.
- Real Manager rollback permits re-registration with single topic/Pub/Sub and exact shared limits, proving mapped-resource/budget/Reader-slot recovery.
- Malformed success descriptors test truncation/schema/zero-ID/size Protocol rejection/closure.
- An owner publishes on an earlier topic and reads a later one; Detach encode/send exceptions and closed-receiver Result failures must not prevent other notifications/later Reader cleanup/Writer reservation or budget/index reuse.

- 在送出請求、成功 body 複製、Descriptor／Attachment 解碼、Publisher attach 與已完成映射後的 counter 開啟邊界，
  注入 `bad_alloc`；確認錯誤分類、pending 歸零、撤銷事件、本機 topology 移除，以及未知結果關閉會話並拒絕重試。
- 真正的 BusManager 處理撤銷後，在單一 topic／Publisher／Subscriber 名額及精確共享預算下重新註冊成功，
  驗證部分映射後的回滾、預算及 reader 名額可重用。
- 畸形 Advertise 成功回覆涵蓋截斷、schema 不符、零 ID、非法尺寸；確認 Protocol 與會話關閉。
- 同一 owner 在前面 topic 發布、後面 topic 持有 reader token；在 Detach 編碼／送出注入例外，
  另驗已關閉接收者的 send Result 失敗。確認其他接收者仍收到通知、後面 topic 的 token 清零、
  Writer 可重新保留位置，以及 Publisher 名額、共享預算、reader index 可重用。

Quiescent fixtures simulate confirmed exit, not actual OS OOM. Existing process tests cover real crash/heartbeat/recovery. Bounded communication/CTest waits do not prove all allocations/schedules.

新測試以 quiescent fixture 模擬已確認退出，沒有假稱真的耗盡 OS 記憶體；
真正子程序崩潰、heartbeat 期間占用及回收由既有 `message_bus.module` 驗證。
所有通訊等待有期限，CTest 另有總期限；不構成所有配置點或所有排程的形式化證明。

<a id="本輪建置與測試結果"></a>

### Recorded build/test results / 本輪建置與測試結果

Windows x64/MSVC 19.51.36257/SDK 10.0.26100.0/CMake 4.3.2/Ninja 1.13.2 related rebuilds without warnings; six Bus suites plus Module process **7/7** each (24.61 s/20.27 s). Both configs isolate injection defines to cleanup tests. No other suites/native Linux/macOS.

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
相關 Debug／Release target 重新建置通過，建置輸出未見編譯警告。
六項 `message_bus.*`（含新 cleanup）及 `module.process` 各 **7／7** 通過，
Debug 24.61 秒、Release 20.27 秒。已核對兩種組態的注入 define 只屬於 cleanup 測試 target。
未執行全專案其餘測試；Linux／macOS 未建置或執行。

<a id="message-storage-存取契約接線2026-10-04"></a>

## Storage access contract integration (2026-10-04) / Message Storage 存取契約接線（2026-10-04）

Checked array/generated operations return Results and examples/tests check them. Invalid []/expired facades/empty WriteLoan access is fatal via contract_violation, without AccessError catches. Standard exception boundaries remain. That stage only changed access; later Bus error/v2 contracts are above.

MessageArray／生成訊息的 checked 操作改用 Result，資料填寫範例及測試已檢查並傳遞結果。
借用內的越界 `[]`、失效 facade 及無效 WriteLoan 解參照屬於 programmer error，透過
`vinc::contract_violation()` 終止當前程序。AccessError catch 轉接已移除；原有標準例外邊界保留。
該輪僅調整存取契約；後續資料面錯誤遷移與控制協定 v2 的現況見上方統一錯誤介面。

<a id="待驗證疑慮建置負載下的程序測試逾時"></a>

### Unresolved concern: timeout under concurrent builds / 待驗證疑慮：建置負載下的程序測試逾時

Recorded 2026-10-04, Debug full tests alongside Release compilation produced one CommandTimeout in Bus Module, other 34 passed. Final sequential full Debug/Release **35/35** passed (case 11.79 s/5.15 s). Repeated success does not establish load as the cause; no timeout/synchronization change. If repeated, record command stage/scheduling/waits to distinguish budget from lifecycle faults.

2026-10-04 Array 契約驗證時，Debug 全套 CTest 與 Release 建置並行，
`message_bus.module` 曾回報 ModuleSystem CommandTimeout；同輪其餘 34 個測試通過。
停止並行編譯後，最終 Debug／Release 全套各 35／35 通過，該測試分別耗時 11.79／5.15 秒。
目前只能確認重跑通過，尚未證明逾時由主機負載造成；沒有放寬 timeout 或修改同步行為。
若再次出現，建議另行記錄命令階段、程序排程與等待狀態，區分執行預算不足和生命週期問題。

<a id="pluginhost-接線2026-10-06"></a>

## Plugin/Host integration (2026-10-06) / Plugin／Host 接線（2026-10-06）

vinc::message_bus_client contains client/ring data APIs using control codecs without Service/Session implementation. vinc::message_bus adds central Manager/Host adapter with control. Existing public/data/order/wait contracts remain.

`vinc::message_bus_client` 包含 message_bus.cpp、ring_buffer.cpp，供 Plugin 使用資料面與
Client API；只依賴控制 codec，不連入 Control Service／Session 實作。
`vinc::message_bus` 包含中央 BusManager 與 Host adapter runtime.cpp，並相依 client／control。
原公開 API、共享布局、控制封包、排序與等待政策不變。

Host injects owned-Session send/close callbacks via ControlTransport. One Client owns request matching/topology; Plugins create no second Client/socket/receiver. Session overload stays Host-only; ControlTransport is designated internal integration, not public/RuntimeServices expansion. Callbacks retain Session and matching native ABI. Two std::functions may allocate at creation, returning ResourceExhausted on failure. No worker or per-payload serialization layer is added.

Host 建立 Client 時透過 `detail::ControlTransport` 提供 send／close callback，callback 捕捉
Host 所有的 Session。同一 Client 的 Transport 保存配對及拓樸；Plugin 不建立第二個 Client、
第二條 Socket 或接收執行緒。RuntimeAccess 的 Session overload 僅存在於 Host adapter，
ControlTransport overload 為指定框架接線，不是新的使用者 API 或 RuntimeServices 能力。
callbacks 必須保留 Session 所有權，且與 Plugin／Client 的相同建置 ABI 一起使用。
兩個 std::function 在 Client 建立時可能配置，失敗回 ResourceExhausted；不新增 worker，
不為共享資料的 publish／copy 增加傳輸或序列化層。

<a id="context-批次接線驗證2026-10-06"></a>

## Context batch integration validation (2026-10-06) / Context 批次接線驗證（2026-10-06）

Bounded source heads/loss/callbacks/removed public next passed Windows regressions, recorded centrally in [Context validation](../module_system/README.md#contextpump-實作驗證2026-10-06). Ring layout/Writer waiting were preserved in that stage.

固定上限讀取、覆蓋統計、callback 與移除公開 next 的接線已納入 Windows Debug／Release
回歸；完整案例、結果與平台限制統一記錄於
[Module System Context 驗證](../module_system/README.md#contextpump-實作驗證2026-10-06)。
Bus 原有 ring buffer 共享布局及 writer 等待政策維持。

<a id="中央共享資源名稱服務2026-10-08"></a>

## Central resource-name service (2026-10-08) / 中央共享資源名稱服務（2026-10-08）

Production Manager injects private ResourceNames into BusManager. Counter/control/data names reserve persistent records before SharedMemory creation; retire only after unlink success/already absent or Windows retirement. Incomplete cleanup stays recorded for next startup. Standalone uninjected tests use random names without cross-crash cleanup guarantees. No payload/order/wait/Reader change or per-message file I/O; see [Instance](../module_system/INSTANCE.md).

正式 Manager 組裝時向 `BusManager::create()` 注入 common 的私有 `ResourceNames`。
Topic counter、RingBuffer control／data 都先 reserve 留下紀錄，再建立 OSAL SharedMemory；
建立者 unlink 成功／名稱已不存在或 Windows 退役時，才 retire 記錄。清理未完成則保留供下次中央處理。
無注入的獨立 Bus 測試仍採原有隨機名稱；這不提供跨中央崩潰的持久清理保證。
本接線不改 payload、排序、等待或 Reader 回收協定，也不增加每筆 pub/sub 的檔案操作。
跨世代與名稱生命週期契約見 [Module System INSTANCE.md](../module_system/INSTANCE.md)。

<a id="recorder-探索及接收上限2026-10-12"></a>

## Recorder discovery and receive limits (2026-10-12) / Recorder 探索及接收上限（2026-10-12）

Client/Context list_topics(after) returns topics with active Publishers/hash/count. Bus v2 op 0x0207 uses 32-entry sorted cursor pages; older endpoints reject unknown operations. No payload/global atomic topology snapshot.

新增 Client／Context `list_topics(after)`，回傳有活動 Publisher 的 topic/hash/Publisher 數。
控制協定新增 v2 operation 0x0207，每頁至多 32 筆、字典序游標，舊端點拒絕不支援的 operation；
不傳 payload，也不保證多頁跨拓樸變更的原子快照。

Subscriber max_message_bytes defaults 0. Nonzero checks before allocation; callback skips oversized images/advances source/increments stats, latest returns CapacityExceeded without cursor change. Validation/leases/Writer waits remain; see [Bag](../bag/README.md).

SubscriberOptions 新增 max_message_bytes，0 保留既有行為。非零時在接收 allocation 前檢查，
callback 路徑略過過大映像並推進 cursor／記錄 oversized_messages；get_last 回 CapacityExceeded，
不改 callback cursor。正常訊息仍走原布局驗證及 Reader lease 複製，writer 等待政策不變。
Recorder 的使用邊界與驗證见 [Bag README](../bag/README.md)。

<a id="雙時間發布-metadata2026-10-13"></a>

## Dual publication timestamps (2026-10-13) / 雙時間發布 metadata（2026-10-13）

- Push/commit sample Unix and monotonic time after copy/seal, write metadata, then Ready/head. Samples are sequential, not an atomic clock snapshot; OS can pause before publication.
- Every publication overwrites both times, without changing local push source. Loan reservation does not sample and reception does not replace with receive time. Copies retain times; payload serialization does not transport/overwrite routing metadata.
- Latest/merge/batch compare monotonic time with Publisher rank ties, not wall-clock/global FIFO.
- Unrepresentable UTC returns TimestampUnavailable/Push or Commit with Common cause and no new head. Push rolls back reservation for retry. Post-seal commit failure revokes loan access but leaves outstanding state until Publisher close, without hidden cancel/reloan.
- The dual-time stage introduced ring v2 by consuming one reserved word; slot 64/header/Reader dimensions remained. Later diagnostics upgraded the current ring to v3. Control/Storage stay v2, schema unchanged; protection/wait policy remains.
- That stage used entry 7/bootstrap 12 for changed BaseMessage layout; current connected builds use entry 8/v13. Rebuild central/Host/Plugin/Python together. Both getters and Python format_utc are available.

- `push(const T&)` 與 `commit(loan)` 在 payload 複製／封存完成後取樣 Unix 與 OSAL 單調時間，
  寫入兩個 metadata 欄位後才發布 Ready／head。它們是先後取樣，並非原子時鐘快照；
  OS 仍可能在取樣與 publication point 間暫停執行緒。
- 每次重新發布都覆寫 Bus 的兩個時間；不沿用來源訊息 metadata，也不修改 push 的本機來源物件。
  借出 loan 時不取發布時間；Subscriber 不改成接收時間。訊息複製保留兩個欄位，
  payload serialize／deserialize 仍不攜帶或替換發布 metadata。
- `get_last`、跨 Publisher 合併與批次讀取只比較單調時間；相同值依 Publisher rank。
  不新增全域嚴格 FIFO 保證，Unix 時間可能倒退。
- Unix 時間無法表示（1970 年以前或超出 uint64 奈秒）回
  `MessageBus::TimestampUnavailable / Push` 或 `Commit`，保留 Common 原因。
  不發布新 head；push 回滾本次 reservation，可重試。commit 在 seal 之後失敗沿用既有政策：
  撤銷 loan 存取，保留 outstanding 狀態，需關閉該 Publisher，不暗中重新借出或取消。
- 該次雙時間變更將 RingBuffer 升為 **v2**（目前診斷版已是 v3，見下文）：使用一個原 reserved word 保存單調時間，slot 仍為 64 bytes，
  header／reader token 尺寸不變。舊／未知布局拒絕。同步、Reader 保護與無限等待政策不變。
  控制協定維持 v2，payload 映像維持 v2，schema hash 不因發布 metadata 改變。
- C++ BaseMessage 布局改變：當時 Plugin entry **7**／bootstrap **v12**，目前為 entry 8／v13，所有中央／Host／Plugin／
  Python 擴充一起重建。Python 同時提供兩個唯讀 getter 及 `vinc.format_utc()`。

UTC formatting uses Common time.hpp/format_utc. [Bag](../bag/README.md) v2 stores both times and rejects old Bags. Param/heartbeat/deadline/contention/Log timing policy was unchanged by this feature.

UTC 格式化使用 common `<vinc/time.hpp>`：`vinc::format_utc(msg.get_message_timestamp())`。
Bag 同步改為 v2，分開保存兩種發布時間；舊 Bag 不相容，見 [Bag](../bag/README.md)。
Param 時間、heartbeat／timeout、碰撞等待計時與 Log 的既有契約不在本輪變更範圍。

<a id="本輪驗證2026-10-13windows-x64"></a>

### Dual-time validation (2026-10-13, Windows x64) / 本輪驗證（2026-10-13，Windows x64）

SDK 10.0.26100.0/CMake 4.3.2/Ninja 1.13.2/Python 3.13.12: full builds in four configurations; related suites:

使用 Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2 與 Python 3.13.12，
四種組態皆完成完整建置；相關 CTest 結果如下：

| Compiler | Configuration | Tests | Repository-relative log |
| --- | --- | --- | --- |
|19.51.36257|Debug|27/27|build/dual-time-1951-debug-tests.log|
|19.51.36257|Release|27/27|build/dual-time-1951-release-tests.log|
|19.44.35229|Debug|27/27|build/dual-time-1944-debug-tests.log|
|19.44.35229|Release|27/27|build/dual-time-1944-release-tests.log|

| 編譯器 | 組態 | 相關測試 | 測試紀錄（相對 repository root） |
| --- | --- | --- | --- |
| MSVC 19.51.36257 | Debug | 27/27 | `build/dual-time-1951-debug-tests.log` |
| MSVC 19.51.36257 | Release | 27/27 | `build/dual-time-1951-release-tests.log` |
| MSVC 19.44.35229 | Debug | 27/27 | `build/dual-time-1944-debug-tests.log` |
| MSVC 19.44.35229 | Release | 27/27 | `build/dual-time-1944-release-tests.log` |

Scope covers Common time/errors, Storage, Bus, generator, Module/Context/Plugin, Python, external SDK, Bag. Check UTC bounds/monotonic ordering through wall rollback, publication/failure rollback, process metadata, old layouts/Bags, recording columns. After final concurrent-metadata assertion,19.51 Debug fully rebuilt and reran 3 suites; other 27-case runs already included it.

範圍包含 Common 時間／錯誤、訊息儲存、Bus、Ribosome、Module 程序／Context／Plugin、
Python、外部 SDK consumer 與 Bag。覆蓋 unsigned UTC 邊界、時鐘倒退時的單調排序、
push／commit 取樣與失敗回滾、跨程序 metadata、舊布局／Bag 版本拒絕及錄製時間欄位。
補入最後一項 RingBuffer 並行 metadata 斷言後，19.51 Debug 再完成完整建置，
並重跑 `common.time`、`message_bus.cleanup`、`message_bus.ring_buffer`，3/3 通過；
其餘三組 27 項回歸包含該斷言。

Only 27 of 76 CTests were run. Debug retains iterator-debug allocation skips; Release scans them. 19.44 retained existing Shell C4127 warnings. Later platform evidence is recorded in the [platform reports](../../../README.md#validation-and-limits).

本輪執行的是 76 項 CTest 中的 27 項相關測試，未宣稱全套重跑。
MSVC Debug 的 `message.access` 仍依既有 iterator-debug 條件略過配置失敗掃描，Release 執行該路徑。
19.44 建置仍有 Shell `tcl_runtime.cpp` 的既有 C4127 警告，本輪未修改該部分。
後續平台結果與測試範圍見[平台驗證報告](../../../README.md#validation-and-limits)。

<a id="唯讀診斷與-reader-計數2026-10-25"></a>

## Read-only diagnostics and Reader counters (2026-10-25) / 唯讀診斷與 Reader 計數（2026-10-25）

inspect_topics(after) returns at most one topic/page with counts/current Publisher ID/owner MID/name/publication/contention/Reader loss/oversized stats. Ordinary authenticated Context queries gain no write authority/socket/singleton. Bus v2 op 0x0208, diagnostic v1 explicitly validates fields/names/count/order/version/truncation/trailing bytes; old peers reject unknown operations without guessing.

Client／Context `inspect_topics(after)` 提供 TopicDiagnosticsPage，每頁最多 1 個 Topic，
包含訂閱數、目前 Publisher ID／owner MID／名稱、發布與碰撞等待統計，以及 Reader 漏資料／超量略過。
一般 Context 透過原有認證控制 Session 查詢；沒有修改權限、額外 Socket 或管理 singleton。
控制 operation **0x0208**（Bus v2），內層 diagnostic version 1。欄位明確編碼，
名稱／筆數／排序／版本／截斷與尾端資料皆驗證；未知新操作由舊端拒絕，不相容猜測。

Current ring layout is **v3**. ReaderSlot stays 64 bytes with three previously reserved SharedWords for lost/overwrite/oversized counts. Tokens/gates remain. Subscribers count confirmed loss; central samples atomically without payload leases. unprotect/clear_reader clear tokens only; after quiescent unsubscribe/confirmed exit central collects stats then reset_reader clears token/counters before index reuse. Old layouts reject, all participants rebuild; Storage v2/hash unchanged.

RingBuffer 共享布局升為 **v3**。ReaderSlot 保持 64 bytes，原保留空間改為三個 SharedWord
記錄 lost_messages／overwrite_events／oversized_messages；token 與 Writer gate 同步不變。
Subscriber 在確認漏資料時更新計數；中央只原子讀取，不取得 payload lease。
`unprotect`／`clear_reader` 只清 token；中央確認 quiescent unsubscribe／程序退出後，
先收集診斷，再以 `reset_reader` 清 token 與計數供 index 重用。
舊／未知布局拒絕，中央／Host／Plugin 必須一起重建；payload 映像 v2 與訊息 hash 不變。

Pages/fields are independently sampled, not atomic snapshots. Retired-source counts persist while Topic exists, not unbounded after pruning. Readers may finish final counters during Publisher retirement, so exits remain best effort. This is state diagnosis, not durable/lossless events; unobserved overruns are not guessed, and summing per-Reader losses does not count unique publications. No per-message control notification/telemetry thread.

每頁與每個欄位是各自取樣，不保證跨頁／跨欄位原子快照。
已收集的退役來源計數在 Topic 存續期間保留；Topic 被 prune 後不保留無界歷史。
Publisher 退役時可能仍有其他 Reader 完成最後一次診斷更新，因此退出邊界為盡力觀察。
這是狀態診斷，不是可靠／持久事件紀錄；Subscriber 尚未讀取時不提前宣告其漏失量。
多 Reader 的 lost 相加不等於唯一發布訊息數。沒有新增每筆控制通知或一般運作的 telemetry 執行緒。

Protection/copy/infinite-wait/cursor/loan/heartbeat/confirmed-exit recovery policies remain. [UI](../ui/README.md#topic-monitor-契約2026-10-25) owns Hz/colors/recovery/preview budgets.

此功能不改變 Reader 保護複製、無限等待、游標、loan、heartbeat 或確認退出後才能回收的政策。
UI 的 Hz／燈號／恢復與內容預覽預算由 [UI 元件](../ui/README.md#topic-monitor-契約2026-10-25) 定義。

Related Debug/Release Bus/Context/Module/UI suites passed; exact scope/logs/platform limitations are in [UI validation](../ui/README.md#topic-monitor-實際驗證).

本輪 Debug／Release 的 Bus、Context、Module 與 UI 相關回歸均通過；完整測試範圍、
紀錄與未驗證平台統一見 [UI 驗證](../ui/README.md#topic-monitor-實際驗證)。

<a id="read-only-connections"></a>

## Read-only connections / 唯讀連線查詢（2026-10-27）

`Client::list_topic_connections(ConnectionCursor after, Deadline)` returns
`Result<TopicConnectionsPage>`. The exclusive cursor sorts by (topic name, role,
registration ID); empty topic starts/ends the scan. Up to 32 raw registrations per
page keep the existing 16 KiB control envelope bounded, including pagination within
one large topic. Roles are Publisher=1 and Subscriber=2. Each record includes the
schema hash and authenticated owner MID. Subscriber-only topics are included.
There is no shared-memory mapping, statistics sampling, payload read, new subscriber,
or background topology timer. Existing `list_topics` / `inspect_topics` behavior is unchanged.

新增查詢以（Topic 完整名稱、角色、登記 ID）遞增排除式游標分頁，每頁最多 32 筆，
單一 Topic 也可跨頁；空 Topic 為起點／終點。包含 Subscriber-only Topic、schema 與
已驗證 owner MID。不 mapping 資料、不取樣統計、不讀訊息、不新增訂閱或背景計時器。
既有 `list_topics`／`inspect_topics` 行為不變。

Operation 0x0209 uses query payload version 1 and validates cursors, row count, roles,
MIDs, ordering, trailing data and version. Registry changes between pages may omit
newly inserted earlier keys; this is a best-effort current scan, not a global snapshot.
A timed-out read keeps the Session usable. A retired-query watermark discards old unclaimed read-only replies (including duplicates)
after validating their response envelope. Timeout releases pending capacity immediately;
lost responses cannot accumulate pending slots. Mutation reply handling stays strict.
No automatic retry is performed. Query send backpressure and central queue saturation
return failures instead of applying the mutation-timeout shutdown policy. A congested
central response may be dropped, leaving the caller to time out. Malformed control
frames and genuinely lost Sessions retain existing protocol/lifecycle handling.

新操作 0x0209／payload v1 驗證游標、筆數、角色、MID、順序、尾端資料與版本。
分頁間登記可能變動，早於游標的新登記可能等下次才看到。唯讀逾時保留 Session；
單一退休查詢水位在驗證 envelope 後丟棄舊的無等待者唯讀回覆（含重複回覆）。
逾時立即釋放 pending 額度，不因遺失回覆持續累積；mutation 回覆仍嚴格配對。不自動重試。傳送壅塞／中央佇列滿不套用 mutation
逾時的關閉政策；中央回覆壅塞可丟棄，由呼叫端逾時。畸形控制封包及真正斷線仍照既有契約處理。

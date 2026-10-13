# 訊息儲存與 MessageArray

本元件為 Ribosome 生成的訊息 class 提供儲存、陣列欄位與可搬移格式。
正常入口是 `vinc::make_message<生成的訊息型別>()`；`MessageArray` 是訊息裡的陣列容器。
獨立使用容器不會取得發布資格。使用 C++23，目前限定 64-bit 平台。

```cpp
#include <vinc_messages/Frame.hpp> // 示意；Frame 定義在測試 fixtures

auto msg = vinc::make_message<vinc::Frame>();
msg->data().sequence = 42;
auto result = msg->data().detections.resize(2);
if (!result) {
    // Inspect result.error().is(vinc::message::ErrorCode::CapacityExceeded), for example.
    return;
}
msg->data().detections[0].data().id = 7;

auto copied = msg->data().detections;  // 深複製訊息內容，獨立本機儲存
auto& ref = msg->data().detections;    // 原訊息的陣列欄位
auto* ptr = &msg->data().detections;   // 指向原欄位
```

## 元件目錄

### 建置待處理事項（2026-10-06 CMake 重審）

**來源確認，尚未缺檔實測：** `tests/CMakeLists.txt` 的 fixture 生成規則只宣告
`generation.stamp`，未將產出的訊息 `.hpp` 列為 OUTPUT／BYPRODUCTS。
保留 stamp 而遺失個別標頭時，建置圖不會因該檔案遺失而重新執行 Ribosome，
後續編譯可能直接失敗。正式 `vinc_messages` 的每次重生政策不受此項影響。
尚未修復；建議補齊所有 fixture 標頭的生成宣告，並驗證缺檔重建。


本元件位於 `src/vinc/components/message_storage/`，自行維護 include、src、tests 與 CMake。
連結入口維持 `vinc::message_storage`，公開標頭維持 `<vinc/message/...>`；
`components` 是目錄分類，不加入 C++ 命名空間，也不改變 API 的公開程度。

`BaseMessage` 已歸入 `<vinc/message/base_message.hpp>`；容器與生成訊息支援維持本元件所有。
映像 v2 的固定欄位位置與寬度集中於 `include/vinc/message/detail/image_format.hpp`；
這些是格式常數，不是系統設定。
固定記憶體綁定的框架接線契約為 `<vinc/message/detail/framework_access.hpp>`，不屬於一般應用入口。

### 實作閱讀順序

- `storage.hpp`：型別／schema、陣列值與綁定語意、代理參照與迭代器、不可變 Snapshot。
- `storage.cpp`：先看 `calculate_layout()`、`render_into()` 與 `rebuild()`，了解暫存布局、
  編碼與最後提交；`prepare_committed_plan()` 僅整理已完成編碼的暫存 metadata。
- 外部映像驗證依序執行 header、root、descriptor／payload、子節點唯一所有權、根可達性檢查；
  `validate_schema()` 再比對生成訊息的欄位契約。
- `adopt_validated_record()` 接管接收副本；`populate_readonly_plan()` 建立複製用的本機索引；
  `prepare_published_image()` 在首次寫入目的區前完成提交所需的 metadata 配置。
- `generated.hpp`：Ribosome class 的純量／字串欄位、複製與移動、序列化轉接。

先前的 2026-10-04 可讀性整理保留 v2 欄位數值、公開 API、驗證與提交順序，以及既有錯誤／例外邊界。
測試中的損壞映像 offset 刻意獨立於實作常數，避免編碼器與測試一起接受意外的格式變動。

## 三條資料路徑

**文件待修正（2026-10-06 benchmark 閱讀確認）：** 本節稍後的
`Module::message_bus()` 取得方式已過時；目前應從 BaseModule 繼承的
`context.advertise<T>()`／`context.subscribe<T>()` 使用服務，依 Module System 的 Context 契約。
後續需同步清理舊說明；本輪未修改 Storage API。

| 路徑 | 已實作的訊息儲存行為 |
| --- | --- |
| 接收共享訊息 | 生成類別的 `from_bytes()` 複製完整映像一次，再驗證副本並建立本機欄位存取介面；解除來源映射後可繼續讀寫 |
| 借用共享訊息後填寫 | 私有 `FrameworkAccess::borrow_message<T>()` 綁定固定 byte 上限；各欄位直接寫入共享資料；`seal()` 驗證並撤銷寫入，不複製或搬移 payload |
| 提交本機訊息 | 私有 `FrameworkAccess::push()` 先計算緊密布局與容量，再將有效資料直接寫到目的共享區；超限保留來源與目的區原內容 |

**整合邊界：** 這些借用、提交入口維持在框架專用 `include/vinc/message/detail/framework_access.hpp`，由
[Message Bus 的 Publisher／Subscriber](../../message_bus/README.md) 接上共享 RingBuffer、slot 同步與失聯回收。
Message Bus 接收端只複製完整映像一次，透過 `adopt_record` 接管該本機 buffer 後再驗證及建構 class。
一般使用者從 Module 的 `message_bus()` 取得 Publisher／Subscriber，不直接建立共享訊息。

框架傳入的 owner 必須保留映射及 slot lease，並保證借用時獨佔寫入。
接收複製期間必須保護來源，完成後才能讓發布者重用 slot。
`seal()` 回傳的 Snapshot 持有 owner；它不執行跨程序 atomic 或發布通知。
借用訊息銷毀、明確 revoke 或成功 seal，都會結束可寫生命週期。

## 訊息與欄位的所有權

- 每個生成訊息的所有純量、子訊息、陣列及字串，綁定到同一份儲存。
  C++ class／虛擬函式表／存取 wrapper 都位於本機，未放入共享映像。
- 複製持有內容的訊息 class、`Data_t`、`MessageArray` 或 `MessageString`，取得獨立本機內容。
  複製 `[]` 回傳的輕量 proxy 則仍指向原資料；要取得元素副本，使用 `at()` 或 proxy 的 `copy()`。
- 整筆陣列、字串、生成訊息及 `Data_t` 的 copy／move assignment 已刪除。
  用 `assign()` 回傳 `Result<void>`，保留目的 binding 與容量預算；純量元素 `array[i] = value` 保留。
- 移動建構獨立物件可移交儲存；從訊息內的 owning 欄位移出會複製到本機，保留父訊息結構。
  複製／移動 proxy 不會移走訊息內容。
- `auto&`／指標仍可引用實際存在的 owning 欄位。巢狀 `[]` 改回傳 proxy value，使用
  `auto element = array[i]`，不能以 `auto&` 接住暫時 proxy；range-for 同理使用 `auto element`。
  proxy 保留本機儲存與 metadata；已刪除節點仍會失效，也不會延長共享借用的可寫 lease。
  不可解參照已被銷毀的 C++ owning 物件指標。
- 訊息 metadata（topic、發布者、時間、topic count、Publisher instance）屬於 `BaseMessage`；
  payload 格式不攜帶這些路由資料，由 Message Bus 在接收端補上。子訊息不具有獨立發布通道。

## 支援型別與操作

訊息陣列支援固定寬度整數、IEEE 754 float／double、bool、生成的訊息 class、
`MessageString` 與巢狀 `MessageArray`。布局規則由 Ribosome 生成，並遞迴驗證。
bool 每個值使用一個 byte（0／1），`std::vector<bool>` 匯入逐元素編碼。

`MessageArray` 提供大小／容量查詢、resize、reserve、clear、shrink_to_fit、
push_back、emplace_back、pop_back、set、索引、迭代與 vector 匯入／複製回 vector。
`resize` 的新增數值為零，新增訊息的欄位初始化為零或空內容；`.msg` 常數不占 payload。
`at()` 回傳 `Result<T>`；對巢狀陣列／訊息／字串會建立獨立本機副本。
`[]` 回傳純量值／純量寫入 proxy 或巢狀輕量 proxy，不配置資源、不複製 payload。

一般 C++ 型別也可以獨立使用：

```cpp
vinc::MessageArray<std::string> names;
vinc::MessageArray<std::unique_ptr<MyClass>> objects;
```

這類本機物件使用標準容器管理 C++ 建構／移動／析構，不允許放進訊息映像。
複製行為取決於元素：指標只複製指標，move-only 元素的容器不能作元素複製。
本機物件容器可用 `assign(std::move(source))` 移交內容；move-only 元素沒有 `at()` 的複製入口，
可在遵守索引契約下以 `[]` 取得參照。
任意使用者型別拋出的例外遵循該型別與標準容器的行為。

### 存取介面與 vector 的差異

- 訊息純量欄位使用 `Scalar<T>`，支援賦值、取值與隱式轉換；`get()` 可取得明確的原生 T。
  複製 Scalar 會取出獨立本機值。它不是原生 `T&`，需要精確 T 的模板呼叫可使用 `get()`。
- 訊息純量陣列的非 const 索引是 proxy；const 索引取值。未提供原生 `T* data()`。
  `auto element = array[i]` 可能取得 proxy；要明確複製純量，可用 `at()` 或轉成 T。
- 巢狀索引不再有 wrapper cache；record 的 `.data()` 回傳生成的欄位 proxy 集合。
  第一次存取也不配置記憶體或初始化 schema。const 路徑產生唯讀 proxy，複製後仍不可寫。
- 純量元素 proxy 和 iterator 保存陣列 revision，結構變更後失效。
  巢狀 proxy 以 node handle 綁定；只要原子節點仍存在就可繼續使用，刪除／替換該子樹後失效。
  借用撤銷或封存後，其所有可寫 facade 都失效；存取失效 facade 屬於契約違反。
- `MessageArrayView<T>` 的 `[]` 同樣回傳值或唯讀 proxy；`at()` 才做獨立複製。
  immutable view 的 `capacity()` 回報映像內記錄的容量，封存映像可大於 `size()`。
  接收整筆訊息仍使用生成類別的 `from_bytes()`。
- 這些 facade 並非完整的 STL vector 替代品；未實作 insert／erase／完整 random-access
  iterator 介面，不承諾所有 STL 演算法可直接使用。

## 容量、失敗與效能範圍

固定上限涵蓋整筆訊息的 header、root 列表、descriptor、索引、有效資料、
預留容量與 8-byte 對齊。多個欄位共享同一份預算。`usage()` 提供
`compact_bytes`、`allocated_bytes`、`limit_bytes`；本機的 limit 為零，表示沒有 publisher 限制。

結構變更先在暫存區完成布局，再提交。失敗保留原內容。固定綁定的 scratch_limit
限制每份 payload 暫存區，metadata、C++ wrapper 與使用者本機副本另計。
保留容量內的純量填寫直接更新原空間；需要重排時，可能搬移整筆有效內容。
此版本每個陣列、結構及純量欄位都有 descriptor，結構陣列的 metadata 成本尚未最佳化。
不宣稱大型點雲吞吐量或低延遲保證。

本機提交／snapshot 會移除閒置容量；共享借用 seal 保留當前布局及預留容量，
藉此不做整筆 payload copy。可在填寫期間明確 `compact()` 釋放預留空間；
這會搬移資料。此為 zero-copy commit 下的容量／搬移取捨。

### 存取與失敗契約（2026-10-04）

| API | 行為 |
| --- | --- |
| `array[i]` | 呼叫端保證 index 合法；保留 Debug／Release 檢查，違反時進入 Common fatal contract handler，終止當前程序 |
| `array.at(i)` | 合法回傳 `Result<T>` 的值／獨立副本；非法 index 回傳 `OutOfRange` |
| `array.set(i, value)` | checked write，回傳 `Result<void>`；索引、容量等預期失敗可由呼叫端處理 |
| `destination.assign(source)` | 整筆替換的 Result 入口，保留目的 binding；不提供整筆 `operator=` |
| 純量 `array[i] = value` | 一般 assignment 語意；固定寬度寫入不配置空間，無 Result |
| `proxy.copy()` | 明確建立獨立本機副本，回傳 Result |

`at()`／`set()` 能回報索引錯誤，不代表失效物件可以繼續使用；已撤銷借用、失效 handle／iterator
或 moved-from facade 的資料存取屬於 programmer error。底層 detail 的驗證 Result 保留給框架接線。
不把可恢復的容量、大小或配置失敗送進 contract handler。

```cpp
auto element = msg->data().detections[0]; // 借用，不配置、不複製
auto alias = element;                   // 仍指向同一筆資料
alias.data().id = 7;

auto independent = msg->data().detections.at(0);
if (!independent) {
    return;
}

independent->data().id = 9; // 不影響原資料
auto replaced = msg->data().detections.set(0, *independent);
if (!replaced) {
    return;
}
```

已移除框架 `AccessError`。既有配置邊界繼續將 `std::bad_alloc`／`std::length_error` 轉成 Result；
一般 C++ 建構／複製、標準容器、第三方及使用者自訂元素仍可能拋例外，未全面加上 `noexcept`
或停用編譯器例外。普通本機物件陣列的元素 `=` 沿用該型別的 C++ 行為與例外保證。
本機及共享 facade 的寫入需由呼叫端序列化，不保證與讀取並行安全；immutable Snapshot 可並行讀取。
本元件不宣稱 lock-free。

## 統一錯誤介面

`<vinc/message/error.hpp>` 提供 `std::int32_t` 的 `message::ErrorCode` 與
`message::ErrorOperation`；Result／Error 使用 common 的型別，不另定元件 payload。
原有九個錯誤數值 0–8 明確保留；`CapacityExceeded = 0` 仍代表失敗，成功只由
Result 的 value 狀態表示。程式以 `error.is(message::ErrorCode::...)` 判斷，避免裸數字
與其他 domain 混淆。靜態 C++ catalog 經 CMake provider 接到 `vinc::errors`，
文字查詢統一呼叫 `vinc::error_to_string(result.error())`；使用查詢的最終程式須連結該 target。

錯誤標記實際失敗的儲存操作；內層已提供 operation 時直接傳遞，不因同元件的呼叫層數
重複包裝。底層大小檢查、布局、重建、驗證及讀寫以 Result 逐層傳遞預期失敗；
配置邊界仍將 `std::bad_alloc`／`std::length_error` 轉為 ResourceExhausted／SizeOverflow。
公開存取現在區分 Result 與契約終止，不再使用 AccessError。
訊息映像 v2 不包含 Error 物件，本次遷移不改變映像欄位、容量或封存語意。

Message Bus 已使用 common Result/Error，以本層分類包裝 Storage 失敗並保留原因鏈；
跨程序控制回覆的版本與傳輸規則見 [Message Bus README](../../message_bus/README.md)。

### 前輪：錯誤型別與查詢遷移驗證（2026-10-04）

Windows x64／MSVC Debug、Release 完整建置成功，包含新錯誤標頭的 SDK first-include
編譯檢查。完整 CTest 各 **34／34** 通過，耗時分別為 77.38 秒與 70.25 秒。
新增 `message.errors` 驗證零號失敗、domain／operation 比對、未知代碼保留、
靜態 catalog、原因鏈及 AccessError payload；既有容量、巢狀陣列、損壞映像、
跨程序映射、生成訊息與 Message Bus 測試也通過。Linux／macOS 本輪未執行；
這些測試不代表既有例外流程已移除，也不是效能量測。

### 前輪：底層 Result 傳遞（2026-10-04）

- 已移除 `src/storage.cpp` 的內部 `Failure`、`require()`／`fail()` 及
  `guarded()` 例外轉接。算術、布局、映像驗證、讀寫、借用與發布的預期失敗直接回傳 Result。
  `with_allocation_errors()` 只處理標準配置／長度例外，並補上尚未設定的 operation。
- 結構修改仍先完成暫存布局與渲染，再提交至原儲存區。幾何容量超過容量／scratch 預算時，
  依原政策改以精確容量重試；配置例外仍交給外層邊界，不新增重試政策。
- `to_vector()` 的純量與巢狀匯出改為檢查既有 Result，不再藉由 `checked(at(...))`
  或公開 `[]` 走預期失敗路徑。巢狀結果仍為獨立本機副本。
- `seal_binding()` 補上 header 長度與借用區域的比較，避免損壞長度在驗證前形成越界 span。
  驗證失敗仍保留原借用狀態；修復 header 後可再次封存。
- 映像 v2 格式、公開簽章、操作代碼及零拷貝封存契約不變。

Windows x64／MSVC 19.51.36257 的 Debug、Release 完整建置成功，包含 SDK 正式標頭
獨立編譯；完整 CTest 各 **34／34** 通過，耗時分別為 **73.81 秒／66.34 秒**。
`message.errors` 新增容量與 scratch 預算的精確容量重試、失敗後完整借用映像及
舊 handle 保留、巢狀匯出深複製、損壞 header 拒絕與再次封存，以及無效／循環 schema
的 Result 診斷測試。既有跨程序映射、生成訊息與 Message Bus 整合亦通過。
靜態檢查確認內部 Failure 控制流程已移除；Linux／macOS 本輪未驗證，未做真實 OOM
故障注入或效能量測，這些結果也不代表公開容器已全面移除例外。

### 本輪：公開容器契約與代理

上述公開語法決策已實作。Ribosome 同步生成 owning `Data_t` 與不配置的 `Data_tRef<ReadOnly>`；
生成訊息、Data_t、MessageString 的整筆替換改用 `assign()`。
`MessageString(text)` 亦已移除，改為預設建構後檢查 `assign(text)` 的結果。
`BaseMessage::serialize()` 及生成實作改回傳 `Result<void>`，呼叫端須檢查。
Message Bus 的無效 WriteLoan 解參照走 contract handler，已移除 AccessError catch 轉接；
資料面錯誤遷移、RingBuffer 同步與映像格式未納入本輪改寫。

`message.access` 新增配置計數、配置失敗注入、proxy／副本、唯讀傳播及已刪除 assignment 的編譯檢查。
storage／generated 測試改以 OSAL 子程序驗證契約終止，當輪使用指定 terminate handler 的退出碼，
後續 RuntimeServices 接線已改驗證直接 fallback 退出碼，見文末。一般崩潰不算通過。
程序有逾時與清理。容量失敗 rollback、損壞映像與跨程序測試持續保留。
最終 Windows x64／MSVC 19.51.36257 Debug、Release 全目標建置成功，沒有編譯警告，
包含 `contract.hpp` 與各 SDK 標頭的獨立編譯。完整 CTest 各 **35／35** 通過，
耗時 **85.82 秒／78.82 秒**；包括新增 `message.access` 與 Ribosome 代理名稱衝突案例。
零配置檢查涵蓋首次及重複巢狀／record／字串存取、readonly view 與迭代；配置失敗注入
確認 `at()` 回傳 ResourceExhausted，一般 copy construction 保留原生 bad_alloc。
這不是效能量測；Linux／macOS 本輪未建置或執行。

較早一次 Debug 全套與 Release 編譯並行時，`message_bus.module` 曾發生命令逾時；
上述最終兩輪都在編譯結束後依序執行並通過。逾時根因未確認，觀察已記於 Message Bus README。

`BaseMessage::deserialize()` 的整數狀態介面已於下方「解碼介面收尾」遷移為 Result。
標準／第三方／使用者例外的全面策略也不屬本輪；不宣稱程式已全面無例外。

## 映像格式 v2

所有整數採 little endian；header 為 64 bytes，保留 v1 的欄位位置，版本為 2。
flags=1 表示緊密映像，flags=2 表示保留預留容量的有效映像。v1 明確拒絕。

Header：magic `VINCARR\0`（offset 0）、version uint32（8）、flags uint32（12）、
total bytes（16）、node count（24）、root count（32）、descriptor table offset（40）、
payload offset（48）、reserved=0（56）；其餘皆 uint64。
後接 uint64 root node IDs。

每個 descriptor 為 48 bytes：payload offset（0）、size（8）、scalar code uint32（16）、
depth uint32（20）、capacity（24）、record schema hash（32）、reserved=0（40）。
純量 code 1..11 依序是 uint8、int8、uint16、int16、uint32、int32、uint64、int64、
float、double、bool。純量 hash=0；record 使用 code=0 與非零 schema hash。
depth=0 為 record 欄位列表，depth=1 為純量／record 陣列，較大 depth 為巢狀陣列。
非純量 payload 為 uint64 child IDs；record 的欄位型別及單一純量的元素數由 schema 驗證。

檢查包含整數溢位、所有區域邊界、唯一所有權、根可達性、循環、零 padding／閒置區、
bool 值、record hash、欄位型別與順序。所有節點都必須屬於根，沒有原生指標。
`Snapshot::copy_from()` 先驗證通用布局；`Snapshot::array<T>()` 再核對所選根的型別，
record／巢狀 record 會遍歷該子樹，驗證欄位順序、型別及 singular 數量後才交付 view。
型別不符回 `TypeMismatch`、結構數量不符回 `InvalidData`，operation 為 `AccessRoot`。
此步可能配置暫存遍歷 metadata，配置失敗由既有邊界回 `ResourceExhausted`；不複製 payload，
不寫入共享的 Image 或建立 mutable cache。每次取得 record view 會重新驗證，成本隨子樹大小增加。
取得 view 後的 `[]` 仍不配置、不深複製；並行取得 view／讀取 Snapshot 的契約不變。
早期格式會隨設計演進，尚未承諾穩定 ABI；64-bit schema hash 用於相容性識別，不是驗證來源真實性的安全機制。

## 驗證入口與歷史紀錄

在 x64 MSVC developer 環境，從 `src`：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --test-dir ../build/windows-msvc-debug --output-on-failure
```

`message.storage` 保留原有陣列／容量／損壞映像測試；`message.generated` 使用實際 Ribosome
生成的 class，驗證工廠、巢狀欄位、深複製、借用、提交、失敗原子性及 OSAL 跨程序傳遞
2 MiB payload。接收程序在不同位址映射，複製一次後解除共享映射，再讀寫本機訊息。
2026-10-04 可讀性整理驗證：Windows x64／MSVC，Debug 與 Release 完整建置均通過，
包含 SDK 正式標頭獨立編譯；兩種組態各有以下 7 個測試通過：

- `ribosome.schema`
- `message.storage`、`message.generated`
- `message_bus.ring_buffer`、`message_bus.module`、`message_bus.shell`
- `sdk.consumer`

本輪未執行其餘測試；Linux／macOS 尚待實機驗證。兩份訊息測試保留原有斷言／檢查呼叫數
及診斷順序，5 個 `.msg` fixture 未修改。

### Components 目錄搬移驗證（2026-10-05）

本輪整包搬至 `components/message_storage`，更新上層 CMake、測試生成模板的相對路徑與
文件引用。公開 `<vinc/message/...>`、`vinc::message_storage`、錯誤表及 API 契約保留。
搬移前後 21 個檔案逐一比對，元件內只有本 README 與測試 CMake 路徑修改；
13 個 C++ 實作、測試與標頭檔的 SHA-256 完全一致。

Windows x64／MSVC Debug、Release 全目標建置成功，包含 SDK 正式標頭獨立編譯。
兩種組態各有 **11／11** 相關測試通過（Debug 26.54 秒、Release 22.15 秒）：

- `common.error_catalog`
- `ribosome.hash`、`ribosome.schema`
- `message.errors`、`message.storage`、`message.generated`、`message.access`
- `message_bus.ring_buffer`、`message_bus.module`、`message_bus.shell`
- `sdk.consumer`

Ninja 在沙箱內兩次停在檔案掃描；明確重新配置並改於沙箱外建置後完成上述驗證。
本輪未重跑其餘測試，也未執行 Linux／macOS 驗證；耗時不是效能基準。

## 解碼介面收尾（2026-10-05）

`BaseMessage` 及 `GeneratedMessage` 的兩個 `deserialize()` overload 均回傳
`[[nodiscard]] vinc::Result<void>`，不再回傳 0／-1。這是 C++ API 變更，呼叫端與自訂
override 須一起調整並重新編譯；不提供整數相容入口。

- 生成訊息的不帶 offset 版本只接受剛好一筆完整映像，拒絕尾端多餘資料。
- 帶 offset 版本從指定位置解碼一筆；成功才推進 offset。失敗保留目的 payload、
  binding、容量預算及 offset；成功也不替換 topic、Publisher、timestamp 等發布 metadata。
- offset 超出 buffer 回 `InvalidArgument`；空輸入、header 不足、非法長度與截斷回
  `InvalidData`。入口檢查標記新增的 `Deserialize = 28`，既有代碼不改號。
- `receive_record()`／`replace()` 的 Result 直接傳遞，保留 `TypeMismatch`、
  `CapacityExceeded`、`ResourceExhausted` 等分類與原本的 `ValidateImage`／`Copy`／`Assign`
  操作，不在同元件內增加原因層。失效目的物件仍走 contract handler；既有配置例外邊界不變。
- 解碼沿用先建立驗證後的本機副本，再替換目的內容的流程；正常 Bus 接收仍走
  `from_bytes()`／接管 buffer 的路徑。沒有變更映像 v2、schema hash 或 Bus 協定。
- 完整移除無呼叫端的原生布局 helper `push_raw()`、`pop_raw()`、`push_string()`；
  不新增替代 raw codec。先前 `pop_raw()` 的 offset 加法溢位待辦隨該入口移除而關閉。

### 驗證

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 全目標建置與 SDK 標頭獨立編譯通過，建置紀錄未出現編譯警告。
相關 CTest 各 **12／12** 通過（Debug 25.85 秒、Release 20.28 秒）：
`common.error_catalog`、`ribosome.schema`、四個 `message.*`、五個 `message_bus.*`
及 `sdk.consumer`。本輪未重跑全專案其餘測試；Linux／macOS 未建置或執行。

新增解碼驗證包含 BaseMessage 虛擬派送、完整／連續映像、截斷 header、非法長度、
尾端資料、非零／最大 offset、損壞 magic、schema 不符、固定容量與既有 binding 保留，
以及失敗後目的內容／offset 不變、修正後可再解碼。配置失敗注入驗證接收副本配置
回傳原本的 `ResourceExhausted/Copy`；撤銷借用以子程序驗證契約終止。
沒有窮盡所有配置失敗位置。`Deserialize` 靜態文字查詢亦已驗證，底層 `storage.cpp`
與遷移前逐檔雜湊相同；程式來源掃描確認三個舊 helper 無殘留。

### 2026-10-05 重審修正：Snapshot 型別驗證與配置失敗覆蓋

- **已修正：Snapshot 的 record view 缺少遞迴 schema 驗證。**
  `src/storage.cpp::image_root()` 現在於交付 view 前呼叫同一套 `validate_schema()`。
  mutable receive 路徑仍建立本機 schema 索引；const Snapshot 路徑只讀，不改寫已分享的資料。
  公開簽章、映像 v2 及錯誤碼不變。
- `tests/snapshot_tests.cpp` 的損壞映像先通過通用布局驗證，再確認 typed view 以 Result
  拒絕同寬度錯誤純量型別、交換欄位、巢狀欄位錯誤及 singular size=0。
  另驗證巢狀 record 陣列、空陣列、保留容量的封存映像、多根只驗所選子樹，
  以及四個執行緒各 64 次並行取得／讀取 view。`message.access` 亦驗證首次巢狀 record
  索引及欄位存取零配置。這些測試不是全面的競爭偵測或效能基準。
- **已補配置失敗覆蓋：** `tests/access_tests.cpp` 逐次使第 N 筆配置失敗，每次建立新 fixture，
  直到成功路徑不再觸發注入；每條路徑有 2048 次的測試上限。Release 七條路徑共 **179**
  個失敗點通過：typed view 驗證 5、record `at()`／wrapper 23、固定 loan resize 38、
  deserialize 76、seal 9、push 5、borrow 23。依操作契約檢查 payload、舊 proxy、offset、
  外部 owner 釋放及失敗後重試；seal 失敗不撤銷 loan，成功仍引用原 backing。
  borrow 建構失敗時尚未交付 loan，允許初始化已寫入 backing，但必須釋放 owner 並可重試。
  本次未發現上述路徑的回滾問題；不宣稱窮盡所有輸入或證明不存在資源洩漏。

Windows x64／MSVC 19.51.36257，Debug／Release 相關 target 重新建置通過，未出現編譯警告。
四個 `message.*` 加上 `message_bus.error_process`、`message_bus.ring_buffer`、
`message_bus.module` 各 **7/7** 通過。Debug 執行 schema 驗證的 6 個配置失敗點，
其餘六條逐配置掃描在 MSVC iterator debugging 開啟時明確輸出 SKIP，原因見下方。
未重跑其餘全專案測試；Linux／macOS 未建置或執行。

### 2026-10-13 MSVC 19.44 snapshot 相容性修正

原始 `storage.hpp` 在 MSVC 19.44.35229 出現 C2039／C2447：編譯器將主模板的
`snapshot()` 定義錯誤配對到沒有該 API 的本機物件偏特化。五行、不依賴 VINC／STL
的 constrained partial specialization 最小案例亦失敗；同案例在 19.51.36257 通過。
現已將本機物件偏特化移到主模板 `snapshot()` 定義之後，並留下相容性註解。
逐字比對確認搬移區塊內容不變，沒有改動函式行為、API、布局或錯誤契約。
`tests/access_tests.cpp` 補上編譯期檢查：純量及生成 record 仍有 `snapshot()`，
string、move-only 物件及巢狀本機陣列仍沒有該 API。

19.44 工具鏈從 Microsoft 官方套件下載、核對 SHA-256 後隔離解壓，cl.exe 的
Microsoft 簽章有效。已補齊正式連結庫，以下完整建置使用專案原本的 `/MDd`／`/MD`，
未使用前輪局部實驗的 `/NODEFAULTLIB:OLDNAMES`，亦未修改既有 Visual Studio 安裝。
環境為 Windows x64、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2，
`VINC_BUILD_PYTHON=ON`，CPython 3.13.12。

| 編譯器 | Debug 完整建置／相關測試 | Release 完整建置／相關測試 |
| --- | --- | --- |
| MSVC 19.44.35229 | 通過／21/21 | 通過／21/21 |
| MSVC 19.51.36257 | 通過／21/21 | 通過／21/21 |

四組使用獨立建置目錄；19.44 為全新配置及建置，19.51 為既有目錄的完整目標增量建置。
測試包含 `message.*`、`message_bus.*`、`ribosome.*`、`module.python*`、
`sdk.consumer` 及 `bag.*`，涵蓋生成訊息、跨程序資料路徑、Python 綁定與錄製。
各組 CTest 使用 `-j 1 --output-on-failure`，篩選式為
`^(message[.]|message_bus[.]|ribosome[.]|module[.]python|sdk[.]consumer$|bag[.])`。
Debug 的部分配置失敗掃描仍依既有 MSVC iterator debugging 限制略過，Release 執行對應掃描；
不將本次 21 項相關測試宣稱為全專案 75 項測試全數通過。

19.44 的 Debug／Release 各有 10 筆既有 Shell 模板條件 C4127 警告：
`shell/src/tcl_runtime.cpp` 的五處 `CommandKind` 判斷在一般及測試 target 各編譯一次。
本輪不修改 Shell；19.51 本輪建置日誌未出現 C 系列編譯警告。
Linux／macOS 及其他 19.44 修補版本未驗證。
最小案例、套件來源／雜湊與隔離工具鏈位於 `build/msvc-1944-investigation`；
四組完整建置及測試日誌為 `build/storage-1944-fix-{1944,1951}-{debug,release}-{build,tests}.log`。

## 待處理事項

- **已確認工具鏈限制，例外政策維持原狀：MSVC Debug 的配置失敗。**
  本輪 `record at()` 第 2 筆配置注入會終止程序。安裝的 MSVC 14.51.36231 標準庫
  `vector` 第 672–673 行在 `noexcept` 預設建構函式中呼叫 `_Alloc_proxy()`；
  iterator debugging 啟用時會配置偵錯 metadata，失敗無法由外層 catch 轉成 Result。
  Release 的相應掃描已通過；不能由此承諾 Debug 任意記憶體耗盡皆可恢復。
  測試將 CRT assertion 導向 stderr 並關閉 abort 互動回報，避免故障阻塞測試執行器。
  本輪未修改 STL、停用 Debug iterator checks 或改變框架配置例外政策。
  後續若要承諾所有工具鏈的配置失敗皆可恢復，需在例外／配置器政策專題另行決定，
  不應僅在框架外層增加 catch；標準庫及使用者型別的例外仍依既有契約處理。

### 契約故障診斷接線（2026-10-05）

本機物件陣列、訊息陣列及底層子元素索引檢查使用 `contract_index_violation()`，
在既有越界分支傳入 index／size；成功的 `[]` 仍不配置、不查 reporter，也不改成 Result。
Module 內的 handler 將紀錄提交給 [ModuleSystem](../../module_system/README.md#契約故障共享紀錄)
後退出；獨立使用且未綁定時，依 2026-10-06 已確認政策改為直接 `_Exit(77)`，
略過使用者 terminate handler。其他失效 proxy 等契約保留原因與偵測位置，
沒有可用 index／size 時不虛構數值。`at()`／`set()` 的可恢復錯誤語意不變。

Windows Debug／Release 的 `message.access` 及新增 Module 真實程序越界情境皆通過；
結果與限制見 [ModuleSystem 驗證](../../module_system/README.md#共享故障紀錄驗證)。

RuntimeServices 接線後，`tests/contract_probe.hpp` 改驗證 Common 的 fallback 退出碼；
使用者 terminate handler 採另一個哨兵碼，避免將不相關的 C++ termination 視為契約測試成功。
本輪未改動容器存取或可恢復錯誤語意。

## Bag 格式識別接線（2026-10-12）

Bag 僅使用生成訊息的 Snapshot API 產生 canonical 映像，並引用
`message::detail::format::version` 保存相同的 image version。該常數的框架接線不授權
Bag 修改布局或繞過驗證；schema hash 與映像仍由 Message Storage／Ribosome 維護。

## 發布時間 metadata（2026-10-13）

BaseMessage 的 `get_message_timestamp()` 改為 uint64 Unix nanoseconds，新增
`get_message_monotonic_timestamp()` 保存同主機單調發布時間。兩者由 Bus 填入，唯讀；
新建本機訊息預設皆為 0，是否發布不能只由 UTC 為 0 判斷（epoch 本身是合法日期）。
copy construction 保留兩者；assign／deserialize 保留目的 metadata，payload 映像與 schema hash 不變。
BaseMessage 的本機 C++ 布局變更需所有相連元件重建；發布與相容性契約見
[Message Bus](../../message_bus/README.md#雙時間發布-metadata2026-10-13)。

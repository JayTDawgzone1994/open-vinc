<a id="訊息儲存與-messagearray"></a>

# Message Storage and MessageArray / 訊息儲存與 MessageArray

This component supplies storage, array fields, and relocatable images for Ribosome-generated message classes. Normal entry is `vinc::make_message<GeneratedType>()`; MessageArray is a message-field container. Using it independently does not grant publication eligibility. C++23 and 64-bit platforms are currently required.

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

<a id="元件目錄"></a>

## Component layout / 元件目錄

<a id="建置待處理事項2026-10-06-cmake-重審"></a>

### Outstanding build issue (2026-10-06 review; still present) / 建置待處理事項（2026-10-06 審查；目前仍存在）

**Source-confirmed; missing-header recovery not reproduced:** the test fixture rule declares only generation.stamp, not generated `.hpp` OUTPUT/BYPRODUCTS. With the stamp retained and one header deleted, the graph need not rerun Ribosome and compilation may fail. Production messages regenerate each build and are unaffected. The issue remains: declare fixture outputs and test missing-header recovery.

**來源確認，尚未缺檔實測：** `tests/CMakeLists.txt` 的 fixture 生成規則只宣告
`generation.stamp`，未將產出的訊息 `.hpp` 列為 OUTPUT／BYPRODUCTS。
保留 stamp 而遺失個別標頭時，建置圖不會因該檔案遺失而重新執行 Ribosome，
後續編譯可能直接失敗。正式 `vinc_messages` 的每次重生政策不受此項影響。
尚未修復；建議補齊所有 fixture 標頭的生成宣告，並驗證缺檔重建。

The component owns include/src/tests/CMake under `src/vinc/components/message_storage/`. Link `vinc::message_storage`; headers remain `<vinc/message/...>`. Components is a directory category, not an added namespace or a change to API visibility.

本元件位於 `src/vinc/components/message_storage/`，自行維護 include、src、tests 與 CMake。
連結入口維持 `vinc::message_storage`，公開標頭維持 `<vinc/message/...>`；
`components` 是目錄分類，不加入 C++ 命名空間，也不改變 API 的公開程度。

`BaseMessage` lives in `<vinc/message/base_message.hpp>`. Containers/generated-message support remain here. Fixed v2 offsets/widths live in `include/vinc/message/detail/image_format.hpp`; these are format constants, not system settings. Fixed-memory binding uses designated `detail/framework_access.hpp`, not a general application API.

`BaseMessage` 已歸入 `<vinc/message/base_message.hpp>`；容器與生成訊息支援維持本元件所有。
映像 v2 的固定欄位位置與寬度集中於 `include/vinc/message/detail/image_format.hpp`；
這些是格式常數，不是系統設定。
固定記憶體綁定的框架接線契約為 `<vinc/message/detail/framework_access.hpp>`，不屬於一般應用入口。

<a id="實作閱讀順序"></a>

### Implementation reading order / 實作閱讀順序

- storage.hpp: types/schema, owning/bound arrays, proxies/iterators, immutable Snapshot.
- storage.cpp: calculate_layout, render_into, rebuild for staged layout/encoding/commit; prepare_committed_plan handles already-encoded temporary metadata.
- External validation checks header, roots, descriptors/payload, unique child ownership, and root reachability; validate_schema then checks generated field contracts.
- adopt_validated_record takes the received copy; populate_readonly_plan builds local copy indices; prepare_published_image completes commit metadata allocations before the first destination write.
- generated.hpp: generated scalar/string fields, copy/move, serialization adapters.

- `storage.hpp`：型別／schema、陣列值與綁定語意、代理參照與迭代器、不可變 Snapshot。
- `storage.cpp`：先看 `calculate_layout()`、`render_into()` 與 `rebuild()`，了解暫存布局、
  編碼與最後提交；`prepare_committed_plan()` 僅整理已完成編碼的暫存 metadata。
- 外部映像驗證依序執行 header、root、descriptor／payload、子節點唯一所有權、根可達性檢查；
  `validate_schema()` 再比對生成訊息的欄位契約。
- `adopt_validated_record()` 接管接收副本；`populate_readonly_plan()` 建立複製用的本機索引；
  `prepare_published_image()` 在首次寫入目的區前完成提交所需的 metadata 配置。
- `generated.hpp`：Ribosome class 的純量／字串欄位、複製與移動、序列化轉接。

The 2026-10-04 readability pass retained v2 numbers, APIs, validation/commit order, and exception boundaries. Corrupt-image test offsets intentionally remain independent of implementation constants, preventing encoder and test from accepting the same accidental format change.

先前的 2026-10-04 可讀性整理保留 v2 欄位數值、公開 API、驗證與提交順序，以及既有錯誤／例外邊界。
測試中的損壞映像 offset 刻意獨立於實作常數，避免編碼器與測試一起接受意外的格式變動。

<a id="三條資料路徑"></a>

## Three data paths / 三條資料路徑

**Current service entry:** use inherited `context.advertise<T>()` and `context.subscribe<T>()`, following Module System lifecycle contracts. The former Module::message_bus() description has been retired; this documentation correction changes no Storage API.

**目前服務入口：** 由 BaseModule 繼承的 `context.advertise<T>()`／`context.subscribe<T>()` 取得服務，依 Module System 生命週期契約。先前 `Module::message_bus()` 說明已淘汰；這是文件修正，不變更 Storage API。

| Path | Implemented storage behavior |
| --- | --- |
| Receive shared data | Generated from_bytes copies the complete image once, validates the copy, and builds local access; it remains writable after the source mapping is released |
| Fill a shared loan | Private `FrameworkAccess::borrow_message<T>` binds a fixed byte limit; fields write shared storage directly; seal validates/revokes writing without payload copy or relocation |
| Push a local message | Private FrameworkAccess::push computes compact layout/capacity, then writes valid data directly to shared storage; excess capacity preserves source and destination |

| 路徑 | 已實作的訊息儲存行為 |
| --- | --- |
| 接收共享訊息 | 生成類別的 `from_bytes()` 複製完整映像一次，再驗證副本並建立本機欄位存取介面；解除來源映射後可繼續讀寫 |
| 借用共享訊息後填寫 | 私有 `FrameworkAccess::borrow_message<T>()` 綁定固定 byte 上限；各欄位直接寫入共享資料；`seal()` 驗證並撤銷寫入，不複製或搬移 payload |
| 提交本機訊息 | 私有 `FrameworkAccess::push()` 先計算緊密布局與容量，再將有效資料直接寫到目的共享區；超限保留來源與目的區原內容 |

**Integration boundary:** the designated `include/vinc/message/detail/framework_access.hpp` entry points are connected by [Bus Publishers/Subscribers](../../message_bus/README.md) to RingBuffer synchronization and failure recovery. Bus copies the complete image once, adopts that local buffer through adopt_record, then validates/builds the class. Users obtain handles through inherited Context advertise/subscribe, not by constructing shared messages directly.

**整合邊界：** 這些借用、提交入口維持在框架專用 `include/vinc/message/detail/framework_access.hpp`，由
[Message Bus 的 Publisher／Subscriber](../../message_bus/README.md) 接上共享 RingBuffer、slot 同步與失聯回收。
Message Bus 接收端只複製完整映像一次，透過 `adopt_record` 接管該本機 buffer 後再驗證及建構 class。
一般使用者從繼承的 `context.advertise<T>()`／`context.subscribe<T>()` 取得 Publisher／Subscriber，不直接建立共享訊息。

The supplied owner must retain mapping/slot lease and exclusive loan writing. Protect the source throughout receive-copy; reuse is allowed only after copying finishes. The Snapshot returned by seal retains owner but performs no cross-process atomic publication/notification. Loan destruction, explicit revocation, or successful sealing ends writable lifetime.

框架傳入的 owner 必須保留映射及 slot lease，並保證借用時獨佔寫入。
接收複製期間必須保護來源，完成後才能讓發布者重用 slot。
`seal()` 回傳的 Snapshot 持有 owner；它不執行跨程序 atomic 或發布通知。
借用訊息銷毀、明確 revoke 或成功 seal，都會結束可寫生命週期。

<a id="訊息與欄位的所有權"></a>

## Message and field ownership / 訊息與欄位的所有權

- Scalars, child records, arrays, and strings of one generated message bind to one storage. C++ classes/vtables/access wrappers remain local, outside the image.
- Copying an owning message, Data_t, MessageArray, or MessageString creates independent local content. Copying an indexed proxy retains borrowing; use at() or proxy.copy() for an independent element.
- Whole-array/string/message/Data_t copy/move assignment is deleted. assign() returns `Result<void>`, preserving destination binding/capacity. Scalar array[i] = value remains.
- Move construction of independent values can transfer storage; moving an owning field out of a message copies locally and preserves its parent. Proxy copy/move never moves payload away.
- References/pointers may bind actual owning fields. Nested [] returns a proxy value: use auto element, not auto& to a temporary; range-for follows the same rule. Proxies retain local storage/metadata but do not revive deleted nodes or extend a shared writable lease. Never dereference pointers to destroyed owning C++ objects.
- BaseMessage owns routing metadata (topic, publisher, times, count, Publisher instance); payload images do not carry it. Bus fills it on reception; child records have no separate publication channel.

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

<a id="支援型別與操作"></a>

## Supported types and operations / 支援型別與操作

Message arrays support fixed-width integers, IEEE 754 float/double, bool, generated records, MessageString, and nested MessageArray. Ribosome generates recursively checked layouts. Bool uses one byte per value (0/1); `vector<bool>` imports encode element by element.

訊息陣列支援固定寬度整數、IEEE 754 float／double、bool、生成的訊息 class、
`MessageString` 與巢狀 `MessageArray`。布局規則由 Ribosome 生成，並遞迴驗證。
bool 每個值使用一個 byte（0／1），`std::vector<bool>` 匯入逐元素編碼。

MessageArray supports size/capacity, resize/reserve/clear/shrink_to_fit, push_back/emplace_back/pop_back/set, indexing, iteration, and vector import/export. Resize initializes added numeric values to zero and record fields to zero/empty; .msg constants occupy no payload. at() returns `Result<T>`, creating independent local copies for nested arrays/records/strings. [] returns scalar values/write proxies or lightweight nested proxies, without allocation or payload copy.

`MessageArray` 提供大小／容量查詢、resize、reserve、clear、shrink_to_fit、
push_back、emplace_back、pop_back、set、索引、迭代與 vector 匯入／複製回 vector。
`resize` 的新增數值為零，新增訊息的欄位初始化為零或空內容；`.msg` 常數不占 payload。
`at()` 回傳 `Result<T>`；對巢狀陣列／訊息／字串會建立獨立本機副本。
`[]` 回傳純量值／純量寫入 proxy 或巢狀輕量 proxy，不配置資源、不複製 payload。

Arbitrary C++ types are also supported for independent local use:

一般 C++ 型別也可以獨立使用：

```cpp
vinc::MessageArray<std::string> names;
vinc::MessageArray<std::unique_ptr<MyClass>> objects;
```

Local-object arrays use standard containers for C++ construction/move/destruction and cannot enter message images. Copy semantics depend on elements: pointers copy pointers; move-only elements cannot be element-copied. assign(std::move(source)) transfers local-object contents. Move-only elements have no copying at(); contract-valid [] gives references. User-type exceptions retain their standard container/type behavior.

這類本機物件使用標準容器管理 C++ 建構／移動／析構，不允許放進訊息映像。
複製行為取決於元素：指標只複製指標，move-only 元素的容器不能作元素複製。
本機物件容器可用 `assign(std::move(source))` 移交內容；move-only 元素沒有 `at()` 的複製入口，
可在遵守索引契約下以 `[]` 取得參照。
任意使用者型別拋出的例外遵循該型別與標準容器的行為。

<a id="存取介面與-vector-的差異"></a>

### Differences from vector access / 存取介面與 vector 的差異

- `Scalar<T>` fields support assignment, reading, implicit conversion, and get() for exact native T. Copying extracts an independent value; this is not T&, so exact-type templates may need get().
- Non-const scalar-array indexing is a proxy, const indexing reads a value. No native T* data() exists. auto element can retain a proxy; use at() or conversion for a scalar copy.
- Nested indexing has no wrapper cache; record.data() returns generated field proxies. First access neither allocates nor initializes schema. Const proxies remain read-only when copied.
- Scalar proxies/iterators retain array revision and expire on structural changes. Nested proxies bind node handles and survive while that node exists; subtree deletion/replacement invalidates them. Revocation/sealing invalidates writable facades; using an invalid facade violates contract.
- MessageArrayView [] returns a value/read-only proxy; at() copies. Immutable capacity reports image capacity, potentially larger than size in a sealed image. Whole-message reception still uses generated from_bytes.
- These are not full STL vector replacements: no insert/erase or complete random-access iterator API, and no blanket STL algorithm compatibility promise.

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

<a id="容量失敗與效能範圍"></a>

## Capacity, failure, and performance scope / 容量、失敗與效能範圍

Fixed limits cover the complete image: header, roots, descriptors, indices, data, reserved capacity, and 8-byte alignment. Fields share one budget. usage() reports compact_bytes, allocated_bytes, limit_bytes; local limit=0 means no Publisher restriction.

固定上限涵蓋整筆訊息的 header、root 列表、descriptor、索引、有效資料、
預留容量與 8-byte 對齊。多個欄位共享同一份預算。`usage()` 提供
`compact_bytes`、`allocated_bytes`、`limit_bytes`；本機的 limit 為零，表示沒有 publisher 限制。

Structural changes stage layout before commit and preserve prior contents on failure. Fixed-binding scratch_limit bounds each temporary payload, not metadata, wrappers, or user copies. Scalar writes within reserved capacity update in place; reordering can move the complete live payload. Descriptors exist for each array/record/scalar field and record-array metadata is not optimized. No point-cloud throughput or latency guarantee is claimed.

結構變更先在暫存區完成布局，再提交。失敗保留原內容。固定綁定的 scratch_limit
限制每份 payload 暫存區，metadata、C++ wrapper 與使用者本機副本另計。
保留容量內的純量填寫直接更新原空間；需要重排時，可能搬移整筆有效內容。
此版本每個陣列、結構及純量欄位都有 descriptor，結構陣列的 metadata 成本尚未最佳化。
不宣稱大型點雲吞吐量或低延遲保證。

Local push/snapshot remove unused capacity. Shared seal retains current layout/reserves to avoid a full payload copy. Explicit compact() while writing frees reserves but relocates data: a capacity/relocation trade-off for zero-copy commit.

本機提交／snapshot 會移除閒置容量；共享借用 seal 保留當前布局及預留容量，
藉此不做整筆 payload copy。可在填寫期間明確 `compact()` 釋放預留空間；
這會搬移資料。此為 zero-copy commit 下的容量／搬移取捨。

<a id="存取與失敗契約2026-10-04"></a>

### Access and failure contracts (2026-10-04) / 存取與失敗契約（2026-10-04）

| API | Behavior |
| --- | --- |
| array[i] | Caller guarantees index; Debug/Release checks call the Common fatal handler on violation |
| array.at(i) | `Result<T>` value/independent copy, or OutOfRange |
| array.set(i, value) | Checked `Result<void>` write; caller handles expected index/capacity failure |
| destination.assign(source) | Result-based whole replacement preserving binding; no whole operator= |
| Scalar array[i] = value | Ordinary assignment; fixed-width write allocates nothing and returns no Result |
| proxy.copy() | Explicit independent local copy through Result |

| API | 行為 |
| --- | --- |
| `array[i]` | 呼叫端保證 index 合法；保留 Debug／Release 檢查，違反時進入 Common fatal contract handler，終止當前程序 |
| `array.at(i)` | 合法回傳 `Result<T>` 的值／獨立副本；非法 index 回傳 `OutOfRange` |
| `array.set(i, value)` | checked write，回傳 `Result<void>`；索引、容量等預期失敗可由呼叫端處理 |
| `destination.assign(source)` | 整筆替換的 Result 入口，保留目的 binding；不提供整筆 `operator=` |
| 純量 `array[i] = value` | 一般 assignment 語意；固定寬度寫入不配置空間，無 Result |
| `proxy.copy()` | 明確建立獨立本機副本，回傳 Result |

Checked at/set errors do not make invalid objects usable. Revoked loans, expired handles/iterators, or moved-from facade data access are programmer errors. Detail validation Results exist for framework integration. Recoverable capacity/size/allocation failures do not go to the contract handler.

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

Framework AccessError is removed. Existing allocation boundaries still translate bad_alloc/length_error into Result; ordinary constructors/copies, standard containers, third-party code, and user elements may still throw. No wholesale noexcept or compiler-exception disabling was added. Local-element assignment follows its C++ type. Callers serialize mutable facade access; simultaneous reads/writes are not guaranteed safe. Immutable Snapshots permit concurrent reads. This component does not claim lock-free operation.

已移除框架 `AccessError`。既有配置邊界繼續將 `std::bad_alloc`／`std::length_error` 轉成 Result；
一般 C++ 建構／複製、標準容器、第三方及使用者自訂元素仍可能拋例外，未全面加上 `noexcept`
或停用編譯器例外。普通本機物件陣列的元素 `=` 沿用該型別的 C++ 行為與例外保證。
本機及共享 facade 的寫入需由呼叫端序列化，不保證與讀取並行安全；immutable Snapshot 可並行讀取。
本元件不宣稱 lock-free。

<a id="統一錯誤介面"></a>

## Unified errors / 統一錯誤介面

`<vinc/message/error.hpp>` owns int32 message ErrorCode/Operation; payloads/results are Common types. Nine original code numbers 0–8 remain explicit, including CapacityExceeded=0 as failure; only Result value means success. Use domain-aware error.is(message::ErrorCode::...). CMake providers connect static C++ metadata to vinc::errors. Final programs using error_to_string(result.error()) must link that target.

`<vinc/message/error.hpp>` 提供 `std::int32_t` 的 `message::ErrorCode` 與
`message::ErrorOperation`；Result／Error 使用 common 的型別，不另定元件 payload。
原有九個錯誤數值 0–8 明確保留；`CapacityExceeded = 0` 仍代表失敗，成功只由
Result 的 value 狀態表示。程式以 `error.is(message::ErrorCode::...)` 判斷，避免裸數字
與其他 domain 混淆。靜態 C++ catalog 經 CMake provider 接到 `vinc::errors`，
文字查詢統一呼叫 `vinc::error_to_string(result.error())`；使用查詢的最終程式須連結該 target。

Errors identify the failing storage operation; an existing inner operation passes through without redundant same-component layers. Arithmetic, layout, rebuild, validation, reads/writes propagate expected failures through Result. Allocation boundaries retain bad_alloc/length_error to ResourceExhausted/SizeOverflow conversion. Public access separates Result from contract termination, without AccessError. V2 images contain no Error object; migration changed no format, capacity, or seal semantics.

錯誤標記實際失敗的儲存操作；內層已提供 operation 時直接傳遞，不因同元件的呼叫層數
重複包裝。底層大小檢查、布局、重建、驗證及讀寫以 Result 逐層傳遞預期失敗；
配置邊界仍將 `std::bad_alloc`／`std::length_error` 轉為 ResourceExhausted／SizeOverflow。
公開存取現在區分 Result 與契約終止，不再使用 AccessError。
訊息映像 v2 不包含 Error 物件，本次遷移不改變映像欄位、容量或封存語意。

Bus already wraps Storage failures in its own common Error classification with causes. See [Bus](../../message_bus/README.md) for response versions and cross-process encoding.

Message Bus 已使用 common Result/Error，以本層分類包裝 Storage 失敗並保留原因鏈；
跨程序控制回覆的版本與傳輸規則見 [Message Bus README](../../message_bus/README.md)。

<a id="前輪錯誤型別與查詢遷移驗證2026-10-04"></a>

### Historical error-type/catalog migration (2026-10-04) / 前輪：錯誤型別與查詢遷移驗證（2026-10-04）

Windows x64/MSVC full Debug/Release builds, first-include SDK error-header checks, and full **34/34** CTests passed (77.38 s / 70.25 s). message.errors then checked zero-code failure, domain/operation, unknown codes, static catalog, causes, and the then-existing AccessError payload. Existing capacity/nesting/corruption/mapping/generation/Bus tests passed. No Linux/macOS run or performance measurement; this historical result does not imply exceptions were already removed at that stage.

Windows x64／MSVC Debug、Release 完整建置成功，包含新錯誤標頭的 SDK first-include
編譯檢查。完整 CTest 各 **34／34** 通過，耗時分別為 77.38 秒與 70.25 秒。
新增 `message.errors` 驗證零號失敗、domain／operation 比對、未知代碼保留、
靜態 catalog、原因鏈及 AccessError payload；既有容量、巢狀陣列、損壞映像、
跨程序映射、生成訊息與 Message Bus 測試也通過。Linux／macOS 本輪未執行；
這些測試不代表既有例外流程已移除，也不是效能量測。

<a id="前輪底層-result-傳遞2026-10-04"></a>

### Historical internal Result propagation (2026-10-04) / 前輪：底層 Result 傳遞（2026-10-04）

- Removed storage.cpp Failure, require/fail, and guarded exception translation. Expected arithmetic/layout/validation/read/write/borrow/publish failures return Result. with_allocation_errors catches only standard allocation/length errors and supplies missing operations.
- Structural updates still stage layout/rendering before commit. Geometric capacity beyond image/scratch budgets retries exact capacity under existing policy; allocation exceptions keep the outer boundary, without new retries.
- Scalar/nested to_vector checks Results instead of checked(at(...)) or public [] for expected failures; nested exports remain deep local copies.
- seal_binding compares header length with loan bounds before forming a span. Failed validation preserves the loan; header repair allows resealing.
- V2 format, public signatures, operations, and zero-copy seal contract remained unchanged in that stage.

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

Windows x64/MSVC 19.51.36257 full Debug/Release builds/SDK headers passed; full suites **34/34** each (73.81 s / 66.34 s). Added exact-capacity fallback, failed-loan/old-handle preservation, deep exports, corrupt-header repair/reseal, invalid/cyclic schema Results. Existing mapping/generation/Bus passed. Source checks confirmed Failure removal. No Linux/macOS, actual OOM injection, or benchmark; public exception removal was not yet established by that stage.

Windows x64／MSVC 19.51.36257 的 Debug、Release 完整建置成功，包含 SDK 正式標頭
獨立編譯；完整 CTest 各 **34／34** 通過，耗時分別為 **73.81 秒／66.34 秒**。
`message.errors` 新增容量與 scratch 預算的精確容量重試、失敗後完整借用映像及
舊 handle 保留、巢狀匯出深複製、損壞 header 拒絕與再次封存，以及無效／循環 schema
的 Result 診斷測試。既有跨程序映射、生成訊息與 Message Bus 整合亦通過。
靜態檢查確認內部 Failure 控制流程已移除；Linux／macOS 本輪未驗證，未做真實 OOM
故障注入或效能量測，這些結果也不代表公開容器已全面移除例外。

<a id="本輪公開容器契約與代理"></a>

### Implemented public container and proxy contracts / 本輪：公開容器契約與代理

The public syntax decisions are implemented. Ribosome generates owning Data_t and nonallocating `Data_tRef<ReadOnly>`; messages/Data_t/MessageString use assign for whole replacement. MessageString(text) was removed: default-construct and check assign(text). BaseMessage/generated serialize return `Result<void>`. Invalid Bus WriteLoan dereference is fatal; AccessError catch adapters are removed. That work did not rewrite the Bus data-error layer, RingBuffer synchronization, or image format.

上述公開語法決策已實作。Ribosome 同步生成 owning `Data_t` 與不配置的 `Data_tRef<ReadOnly>`；
生成訊息、Data_t、MessageString 的整筆替換改用 `assign()`。
`MessageString(text)` 亦已移除，改為預設建構後檢查 `assign(text)` 的結果。
`BaseMessage::serialize()` 及生成實作改回傳 `Result<void>`，呼叫端須檢查。
Message Bus 的無效 WriteLoan 解參照走 contract handler，已移除 AccessError catch 轉接；
資料面錯誤遷移、RingBuffer 同步與映像格式未納入本輪改寫。

message.access adds allocation counting/injection, proxy/copy/read-only checks, and deleted-assignment compilation checks. OSAL child probes validate contract exit (originally a terminate sentinel, later direct fallback); unrelated crashes do not pass, and children have timeout/cleanup. Capacity rollback/corrupt-image/cross-process tests remain. Final Windows x64/MSVC 19.51.36257 builds/SDK headers were warning-free; full Debug/Release **35/35** (85.82 s / 78.82 s), including access tests and Ribosome proxy-name collisions. First/repeated nested/record/string/view/iterator access checks allocate nothing. Injected at failure returns ResourceExhausted while ordinary copy construction retains bad_alloc. No benchmark or Linux/macOS build/run.

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

An earlier Debug suite running alongside a Release compile saw a message_bus.module command timeout. The final sequential runs after compilation passed. The root cause is unconfirmed and remains in Bus notes.

較早一次 Debug 全套與 Release 編譯並行時，`message_bus.module` 曾發生命令逾時；
上述最終兩輪都在編譯結束後依序執行並通過。逾時根因未確認，觀察已記於 Message Bus README。

BaseMessage deserialize integer status was subsequently replaced with Result, as documented below. Standard/third-party/user exception policy remains a separate subject; this is not a fully exception-free implementation.

`BaseMessage::deserialize()` 的整數狀態介面已於下方「解碼介面收尾」遷移為 Result。
標準／第三方／使用者例外的全面策略也不屬本輪；不宣稱程式已全面無例外。

<a id="映像格式-v2"></a>

## Image format v2 / 映像格式 v2

All integers are little-endian. The 64-byte header retains v1 field offsets with version=2. Flags 1 means compact; 2 means valid reserved-capacity image. V1 is explicitly rejected.

所有整數採 little endian；header 為 64 bytes，保留 v1 的欄位位置，版本為 2。
flags=1 表示緊密映像，flags=2 表示保留預留容量的有效映像。v1 明確拒絕。

Header: magic `VINCARR\0` at 0; u32 version at 8 and flags at 12; u64 total bytes at 16, nodes at 24, roots at 32, descriptor offset at 40, payload offset at 48, reserved=0 at 56. U64 root node IDs follow.

Header：magic `VINCARR\0`（offset 0）、version uint32（8）、flags uint32（12）、
total bytes（16）、node count（24）、root count（32）、descriptor table offset（40）、
payload offset（48）、reserved=0（56）；其餘皆 uint64。
後接 uint64 root node IDs。

Each 48-byte descriptor contains u64 payload offset(0), size(8), u32 scalar code(16), depth(20), u64 capacity(24), record schema hash(32), reserved=0(40). Codes 1..11 are uint8/int8/uint16/int16/uint32/int32/uint64/int64/float/double/bool. Scalars use hash=0; records code=0/nonzero hash. Depth 0 is record fields, 1 is a scalar/record array, higher depths are nested arrays. Nonscalar payloads contain u64 child IDs; schema validates fields and singular counts.

每個 descriptor 為 48 bytes：payload offset（0）、size（8）、scalar code uint32（16）、
depth uint32（20）、capacity（24）、record schema hash（32）、reserved=0（40）。
純量 code 1..11 依序是 uint8、int8、uint16、int16、uint32、int32、uint64、int64、
float、double、bool。純量 hash=0；record 使用 code=0 與非零 schema hash。
depth=0 為 record 欄位列表，depth=1 為純量／record 陣列，較大 depth 為巢狀陣列。
非純量 payload 為 uint64 child IDs；record 的欄位型別及單一純量的元素數由 schema 驗證。

Validation checks overflow, all bounds, unique ownership, root reachability, cycles, zero padding/unused regions, bool values, record hashes, field types/order; every node belongs to a root, with no native pointers. Snapshot::copy_from checks general layout; `array<T>` then checks root type and recursively validates selected record subtrees/order/types/singular counts. Mismatch returns TypeMismatch, structural counts InvalidData, operation AccessRoot. Traversal may allocate metadata and return ResourceExhausted, but copies no payload, changes no shared Image, and creates no mutable cache. Each record view revalidates with subtree-size cost; subsequent [] remains nonallocating/noncopying and concurrent Snapshot reads remain valid. Early formats may evolve; 64-bit hashes identify compatibility, not trusted origin or stable ABI.

檢查包含整數溢位、所有區域邊界、唯一所有權、根可達性、循環、零 padding／閒置區、
bool 值、record hash、欄位型別與順序。所有節點都必須屬於根，沒有原生指標。
`Snapshot::copy_from()` 先驗證通用布局；`Snapshot::array<T>()` 再核對所選根的型別，
record／巢狀 record 會遍歷該子樹，驗證欄位順序、型別及 singular 數量後才交付 view。
型別不符回 `TypeMismatch`、結構數量不符回 `InvalidData`，operation 為 `AccessRoot`。
此步可能配置暫存遍歷 metadata，配置失敗由既有邊界回 `ResourceExhausted`；不複製 payload，
不寫入共享的 Image 或建立 mutable cache。每次取得 record view 會重新驗證，成本隨子樹大小增加。
取得 view 後的 `[]` 仍不配置、不深複製；並行取得 view／讀取 Snapshot 的契約不變。
早期格式會隨設計演進，尚未承諾穩定 ABI；64-bit schema hash 用於相容性識別，不是驗證來源真實性的安全機制。

<a id="驗證入口與歷史紀錄"></a>

## Test entry points and historical evidence / 驗證入口與歷史紀錄

From `src`, in an x64 MSVC developer environment:

在 x64 MSVC developer 環境，從 `src`：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --test-dir ../build/windows-msvc-debug --output-on-failure
```

message.storage retains array/capacity/corruption cases. Real generated classes in message.generated cover factories, nested fields, deep copies, loan/push, failure atomicity, and 2 MiB transfer via OSAL child mappings at different addresses. The receiver copies once, unmaps, then reads/writes locally. Recorded 2026-10-04 readability validation: full Windows x64/MSVC Debug/Release builds/SDK headers and these seven tests passed in both:

`message.storage` 保留原有陣列／容量／損壞映像測試；`message.generated` 使用實際 Ribosome
生成的 class，驗證工廠、巢狀欄位、深複製、借用、提交、失敗原子性及 OSAL 跨程序傳遞
2 MiB payload。接收程序在不同位址映射，複製一次後解除共享映射，再讀寫本機訊息。
2026-10-04 可讀性整理驗證：Windows x64／MSVC，Debug 與 Release 完整建置均通過，
包含 SDK 正式標頭獨立編譯；兩種組態各有以下 7 個測試通過：

- ribosome.schema
- message.storage, message.generated
- message_bus.ring_buffer, message_bus.module, message_bus.shell
- sdk.consumer

- `ribosome.schema`
- `message.storage`、`message.generated`
- `message_bus.ring_buffer`、`message_bus.module`、`message_bus.shell`
- `sdk.consumer`

Other suites were not run in that round; Linux/macOS needed native validation. Two message tests retained assertion/check counts and diagnostic order; five .msg fixtures were unchanged.

本輪未執行其餘測試；後續平台結果見[平台驗證報告](../../../../README.md#validation-and-limits)。兩份訊息測試保留原有斷言／檢查呼叫數
及診斷順序，5 個 `.msg` fixture 未修改。

<a id="components-目錄搬移驗證2026-10-05"></a>

### Move to Components validation (2026-10-05) / Components 目錄搬移驗證（2026-10-05）

The package moved to components/message_storage with parent CMake, template-relative test paths, and links updated. Headers, target, errors, and API contracts remained. All 21 files were compared; only README/test-CMake paths changed internally, and 13 C++ implementation/test/header SHA-256 values matched exactly.

本輪整包搬至 `components/message_storage`，更新上層 CMake、測試生成模板的相對路徑與
文件引用。公開 `<vinc/message/...>`、`vinc::message_storage`、錯誤表及 API 契約保留。
搬移前後 21 個檔案逐一比對，元件內只有本 README 與測試 CMake 路徑修改；
13 個 C++ 實作、測試與標頭檔的 SHA-256 完全一致。

Full Windows x64/MSVC Debug/Release targets and independent SDK headers passed. Related suites **11/11** each (26.54 s / 22.15 s):

Windows x64／MSVC Debug、Release 全目標建置成功，包含 SDK 正式標頭獨立編譯。
兩種組態各有 **11／11** 相關測試通過（Debug 26.54 秒、Release 22.15 秒）：

- common.error_catalog
- ribosome.hash, ribosome.schema
- message.errors, message.storage, message.generated, message.access
- message_bus.ring_buffer, message_bus.module, message_bus.shell
- sdk.consumer

- `common.error_catalog`
- `ribosome.hash`、`ribosome.schema`
- `message.errors`、`message.storage`、`message.generated`、`message.access`
- `message_bus.ring_buffer`、`message_bus.module`、`message_bus.shell`
- `sdk.consumer`

Ninja stalled twice in sandbox scanning; explicit reconfiguration and a build outside that sandbox completed validation. Other suites and Linux/macOS were not rerun. Times are not performance benchmarks.

Ninja 在沙箱內兩次停在檔案掃描；明確重新配置並改於沙箱外建置後完成上述驗證。
本輪未重跑其餘測試，也未執行 Linux／macOS 驗證；耗時不是效能基準。

<a id="解碼介面收尾2026-10-05"></a>

## Deserialization API closeout (2026-10-05) / 解碼介面收尾（2026-10-05）

Both BaseMessage and GeneratedMessage deserialize overloads return `[[nodiscard]] vinc::Result<void>`, not 0/-1. Callers/custom overrides must migrate and rebuild; no integer compatibility entry remains.

`BaseMessage` 及 `GeneratedMessage` 的兩個 `deserialize()` overload 均回傳
`[[nodiscard]] vinc::Result<void>`，不再回傳 0／-1。這是 C++ API 變更，呼叫端與自訂
override 須一起調整並重新編譯；不提供整數相容入口。

- No-offset generated decoding accepts exactly one complete image, rejecting trailing bytes.
- Offset decoding consumes one image and advances only on success. Failure preserves payload, binding, capacity budget, and offset. Success also retains destination publication metadata.
- Out-of-buffer offset returns InvalidArgument; empty/short headers, bad lengths, truncation return InvalidData. Entry checks use Deserialize=28; existing identifiers remain.
- receive_record/replace Results retain TypeMismatch, CapacityExceeded, ResourceExhausted and ValidateImage/Copy/Assign operations without redundant same-component causes. Invalid destination facades remain fatal; allocation boundaries remain.
- Decoding stages a validated local copy before replacement; ordinary Bus reception still uses from_bytes/adopted buffers. V2/schema/Bus protocols unchanged.
- Unused native-layout push_raw/pop_raw/push_string helpers were fully removed without a replacement raw codec. The pop_raw offset-overflow TODO closed with removal.

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

<a id="驗證"></a>

### Recorded validation / 驗證

Windows x64/MSVC 19.51.36257, SDK 10.0.26100.0, CMake 4.3.2, Ninja 1.13.2: full Debug/Release targets/SDK headers passed, no compile warnings. Related **12/12** each (25.85 s / 20.28 s): common catalog, Ribosome schema, four message.*, five message_bus.*, sdk.consumer. Other suites and Linux/macOS were not run.

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 全目標建置與 SDK 標頭獨立編譯通過，建置紀錄未出現編譯警告。
相關 CTest 各 **12／12** 通過（Debug 25.85 秒、Release 20.28 秒）：
`common.error_catalog`、`ribosome.schema`、四個 `message.*`、五個 `message_bus.*`
及 `sdk.consumer`。本輪未重跑全專案其餘測試；Linux／macOS 未建置或執行。

New decoding tests cover BaseMessage virtual dispatch, single/concatenated images, short headers/bad length/trailing bytes/nonzero/max offsets/bad magic/schema mismatch, fixed capacity/binding preservation, unchanged destination/offset on failure, and repaired retry. Allocation injection retains ResourceExhausted/Copy; revoked loans use child contract probes. Not every allocation site was exhausted. Deserialize text lookup passed; storage.cpp hashes matched pre-migration and source scans found no old helpers.

新增解碼驗證包含 BaseMessage 虛擬派送、完整／連續映像、截斷 header、非法長度、
尾端資料、非零／最大 offset、損壞 magic、schema 不符、固定容量與既有 binding 保留，
以及失敗後目的內容／offset 不變、修正後可再解碼。配置失敗注入驗證接收副本配置
回傳原本的 `ResourceExhausted/Copy`；撤銷借用以子程序驗證契約終止。
沒有窮盡所有配置失敗位置。`Deserialize` 靜態文字查詢亦已驗證，底層 `storage.cpp`
與遷移前逐檔雜湊相同；程式來源掃描確認三個舊 helper 無殘留。

<a id="2026-10-05-重審修正snapshot-型別驗證與配置失敗覆蓋"></a>

### Snapshot schema/allocation coverage review (2026-10-05) / 2026-10-05 重審修正：Snapshot 型別驗證與配置失敗覆蓋

- **Fixed missing recursive record-view schema checks:** image_root calls validate_schema before view delivery. Mutable receive still builds local indices; const Snapshot validation never mutates shared data. Signatures/v2/codes unchanged.
- snapshot_tests first accept general layout, then require typed Result rejection of same-width wrong scalar types, reordered fields, invalid nested fields, and singular size=0. Also cover nested/empty/reserved-capacity arrays, selected-root-only traversal, and four threads each acquiring/reading 64 views. message.access checks first nested indexing/fields allocate nothing; these are not comprehensive race or performance tests.
- **Allocation failure coverage:** each fresh fixture fails allocation N until success no longer encounters injection, with 2048 attempts/path limit. Release passed **179** sites over seven paths: typed-view validation 5, record at/wrapper 23, fixed-loan resize 38, deserialize 76, seal 9, push 5, borrow 23. Checks cover payload/proxies/offset/owner release/retry; failed seal retains the loan, success retains backing. Failed borrow construction may initialize backing before delivery, but must release owner and permit retry. No rollback issue was found in these paths; this is not all inputs or a no-leak proof.

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

Windows x64/MSVC 19.51.36257 related Debug/Release targets rebuilt without warnings; four message.* plus Bus error_process/ring_buffer/module passed **7/7** each. Debug exercised six schema-allocation failure sites; the other six allocation scans explicitly SKIP under MSVC iterator debugging, as explained below. Other suites and Linux/macOS were not run.

Windows x64／MSVC 19.51.36257，Debug／Release 相關 target 重新建置通過，未出現編譯警告。
四個 `message.*` 加上 `message_bus.error_process`、`message_bus.ring_buffer`、
`message_bus.module` 各 **7/7** 通過。Debug 執行 schema 驗證的 6 個配置失敗點，
其餘六條逐配置掃描在 MSVC iterator debugging 開啟時明確輸出 SKIP，原因見下方。
未重跑其餘全專案測試；Linux／macOS 未建置或執行。

<a id="2026-10-13-msvc-1944-snapshot-相容性修正"></a>

### MSVC 19.44 snapshot compatibility fix (2026-10-13) / 2026-10-13 MSVC 19.44 snapshot 相容性修正

MSVC 19.44.35229 reported C2039/C2447, pairing the primary snapshot definition with a local-object specialization that has no snapshot. An independent five-line constrained-specialization reproduction failed on 19.44 and passed on 19.51.36257. The specialization now follows the primary snapshot definition, with a compatibility comment. Moved contents match exactly; behavior/API/layout/errors unchanged. Compile-time tests retain snapshot on scalars/generated records, and exclude string/move-only/nested local objects.

原始 `storage.hpp` 在 MSVC 19.44.35229 出現 C2039／C2447：編譯器將主模板的
`snapshot()` 定義錯誤配對到沒有該 API 的本機物件偏特化。五行、不依賴 VINC／STL
的 constrained partial specialization 最小案例亦失敗；同案例在 19.51.36257 通過。
現已將本機物件偏特化移到主模板 `snapshot()` 定義之後，並留下相容性註解。
逐字比對確認搬移區塊內容不變，沒有改動函式行為、API、布局或錯誤契約。
`tests/access_tests.cpp` 補上編譯期檢查：純量及生成 record 仍有 `snapshot()`，
string、move-only 物件及巢狀本機陣列仍沒有該 API。

The isolated 19.44 toolchain was downloaded from official Microsoft packages, SHA-256 checked, and cl.exe signature validated; proper libraries were included. Full builds use the project /MDd and /MD, not experimental /NODEFAULTLIB:OLDNAMES, without changing installed VS. Environment: Windows x64, SDK 10.0.26100.0, CMake 4.3.2, Ninja 1.13.2, Python enabled with CPython 3.13.12.

19.44 工具鏈從 Microsoft 官方套件下載、核對 SHA-256 後隔離解壓，cl.exe 的
Microsoft 簽章有效。已補齊正式連結庫，以下完整建置使用專案原本的 `/MDd`／`/MD`，
未使用前輪局部實驗的 `/NODEFAULTLIB:OLDNAMES`，亦未修改既有 Visual Studio 安裝。
環境為 Windows x64、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2，
`VINC_BUILD_PYTHON=ON`，CPython 3.13.12。

| Compiler | Debug full build / related tests | Release full build / related tests |
| --- | --- | --- |
| MSVC 19.44.35229 | Passed / 21/21 | Passed / 21/21 |
| MSVC 19.51.36257 | Passed / 21/21 | Passed / 21/21 |

| 編譯器 | Debug 完整建置／相關測試 | Release 完整建置／相關測試 |
| --- | --- | --- |
| MSVC 19.44.35229 | 通過／21/21 | 通過／21/21 |
| MSVC 19.51.36257 | 通過／21/21 | 通過／21/21 |

Four independent build directories were used: fresh 19.44 configurations and incremental full-target 19.51 builds. Tests cover message.*, message_bus.*, ribosome.*, module.python*, sdk.consumer, bag.* with serial CTest `-j 1 --output-on-failure` and filter `^(message[.]|message_bus[.]|ribosome[.]|module[.]python|sdk[.]consumer$|bag[.])`. Debug retains documented iterator-debugging allocation-scan skips; Release runs them. These 21 related tests are not a claim that all 75 project tests passed.

四組使用獨立建置目錄；19.44 為全新配置及建置，19.51 為既有目錄的完整目標增量建置。
測試包含 `message.*`、`message_bus.*`、`ribosome.*`、`module.python*`、
`sdk.consumer` 及 `bag.*`，涵蓋生成訊息、跨程序資料路徑、Python 綁定與錄製。
各組 CTest 使用 `-j 1 --output-on-failure`，篩選式為
`^(message[.]|message_bus[.]|ribosome[.]|module[.]python|sdk[.]consumer$|bag[.])`。
Debug 的部分配置失敗掃描仍依既有 MSVC iterator debugging 限制略過，Release 執行對應掃描；
不將本次 21 項相關測試宣稱為全專案 75 項測試全數通過。

19.44 Debug/Release each emitted ten existing Shell C4127 template-condition warnings: five Tcl CommandKind checks compiled in production/tests. Shell was unchanged; 19.51 logs had no C-series warnings. Linux/macOS and other 19.44 patches were not verified. Reproduction/package hashes/toolchain: build/msvc-1944-investigation. Logs: build/storage-1944-fix-{1944,1951}-{debug, release}-{build, tests}.log.

19.44 的 Debug／Release 各有 10 筆既有 Shell 模板條件 C4127 警告：
`shell/src/tcl_runtime.cpp` 的五處 `CommandKind` 判斷在一般及測試 target 各編譯一次。
本輪不修改 Shell；19.51 本輪建置日誌未出現 C 系列編譯警告。
Linux／macOS 及其他 19.44 修補版本未驗證。
最小案例、套件來源／雜湊與隔離工具鏈位於 `build/msvc-1944-investigation`；
四組完整建置及測試日誌為 `build/storage-1944-fix-{1944,1951}-{debug,release}-{build,tests}.log`。

<a id="待處理事項"></a>

## Outstanding issues / 待處理事項

**Confirmed MSVC Debug allocation limitation; exception policy unchanged:** failing the second allocation in record at terminated the process. Installed STL 14.51.36231 vector lines 672–673 call _Alloc_proxy from a noexcept default constructor; iterator debugging allocates metadata whose failure cannot reach outer catch. Release scans passed, but arbitrary Debug OOM recovery is not guaranteed. Tests direct CRT assertions to stderr and disable interactive abort reporting to avoid blocking. No STL patch, disabled iterator checks, or framework exception-policy change was made. A universal recovery promise needs a separate allocator/exception policy decision, not another outer catch; user/STL exceptions retain existing contracts.

- **已確認工具鏈限制，例外政策維持原狀：MSVC Debug 的配置失敗。**
  本輪 `record at()` 第 2 筆配置注入會終止程序。安裝的 MSVC 14.51.36231 標準庫
  `vector` 第 672–673 行在 `noexcept` 預設建構函式中呼叫 `_Alloc_proxy()`；
  iterator debugging 啟用時會配置偵錯 metadata，失敗無法由外層 catch 轉成 Result。
  Release 的相應掃描已通過；不能由此承諾 Debug 任意記憶體耗盡皆可恢復。
  測試將 CRT assertion 導向 stderr 並關閉 abort 互動回報，避免故障阻塞測試執行器。
  本輪未修改 STL、停用 Debug iterator checks 或改變框架配置例外政策。
  後續若要承諾所有工具鏈的配置失敗皆可恢復，需在例外／配置器政策專題另行決定，
  不應僅在框架外層增加 catch；標準庫及使用者型別的例外仍依既有契約處理。

<a id="契約故障診斷接線2026-10-05"></a>

### Contract diagnostic integration (2026-10-05) / 契約故障診斷接線（2026-10-05）

Local-object/message arrays and child-index checks call contract_index_violation with actual index/size. Successful [] still allocates nothing, reads no reporter, and returns no Result. A bound Module submits to [Module System](../../module_system/README.md#契約故障共享紀錄) then exits; standalone unbound use directly _Exit(77)s under the 2026-10-06 policy, bypassing user terminate handlers. Other expired-proxy contracts retain reason/detection site and never invent index/size. Checked at/set recovery semantics remain.

本機物件陣列、訊息陣列及底層子元素索引檢查使用 `contract_index_violation()`，
在既有越界分支傳入 index／size；成功的 `[]` 仍不配置、不查 reporter，也不改成 Result。
Module 內的 handler 將紀錄提交給 [ModuleSystem](../../module_system/README.md#契約故障共享紀錄)
後退出；獨立使用且未綁定時，依 2026-10-06 已確認政策改為直接 `_Exit(77)`，
略過使用者 terminate handler。其他失效 proxy 等契約保留原因與偵測位置，
沒有可用 index／size 時不虛構數值。`at()`／`set()` 的可恢復錯誤語意不變。

Windows Debug/Release message.access and real child-Module bounds faults passed; see [Module validation](../../module_system/README.md#共享故障紀錄驗證).

Windows Debug／Release 的 `message.access` 及新增 Module 真實程序越界情境皆通過；
結果與限制見 [ModuleSystem 驗證](../../module_system/README.md#共享故障紀錄驗證)。

After RuntimeServices integration, contract_probe.hpp checks the Common fallback exit code and assigns user terminate handlers a different sentinel, so unrelated C++ termination cannot pass. Container access/recoverable failures are unchanged.

RuntimeServices 接線後，`tests/contract_probe.hpp` 改驗證 Common 的 fallback 退出碼；
使用者 terminate handler 採另一個哨兵碼，避免將不相關的 C++ termination 視為契約測試成功。
本輪未改動容器存取或可恢復錯誤語意。

<a id="bag-格式識別接線2026-10-12"></a>

## Bag image-format integration (2026-10-12) / Bag 格式識別接線（2026-10-12）

Bag uses generated Snapshot APIs for canonical images and stores the shared message::detail::format::version. This designated constant access grants no layout mutation or validation bypass; Storage/Ribosome retain schema/image ownership.

Bag 僅使用生成訊息的 Snapshot API 產生 canonical 映像，並引用
`message::detail::format::version` 保存相同的 image version。該常數的框架接線不授權
Bag 修改布局或繞過驗證；schema hash 與映像仍由 Message Storage／Ribosome 維護。

<a id="發布時間-metadata2026-10-13"></a>

## Publication-time metadata (2026-10-13) / 發布時間 metadata（2026-10-13）

BaseMessage get_message_timestamp is uint64 Unix nanoseconds; get_message_monotonic_timestamp holds same-host monotonic publication time. Bus fills both read-only fields; new local messages start at 0, which alone does not prove unpublished state because epoch is valid. Copy construction retains times; assign/deserialize retain destination metadata. Payload/hash unchanged, but changed local C++ layout requires rebuilding connected components. See [Bus](../../message_bus/README.md#雙時間發布-metadata2026-10-13).

BaseMessage 的 `get_message_timestamp()` 改為 uint64 Unix nanoseconds，新增
`get_message_monotonic_timestamp()` 保存同主機單調發布時間。兩者由 Bus 填入，唯讀；
新建本機訊息預設皆為 0，是否發布不能只由 UTC 為 0 判斷（epoch 本身是合法日期）。
copy construction 保留兩者；assign／deserialize 保留目的 metadata，payload 映像與 schema hash 不變。
BaseMessage 的本機 C++ 布局變更需所有相連元件重建；發布與相容性契約見
[Message Bus](../../message_bus/README.md#雙時間發布-metadata2026-10-13)。

<a id="有界本機診斷預覽2026-10-25"></a>

## Bounded local diagnostic previews (2026-10-25) / 有界本機診斷預覽（2026-10-25）

`include/vinc/message/detail/preview.hpp` is designated read-only UI integration, not a new transport. Supply a validated, owned local generated message without concurrent mutation, never an active shared write loan. It traverses existing compile-time ribosome_visit_fields for scalars/strings/arrays/nested records without a runtime schema registry or generator/hash/image changes. Callers bound text bytes, elements/array, total nodes, depth, and string bytes; truncation marks omissions/original lengths. Text is human diagnostics, not guaranteed JSON/round-trip data. Non-ASCII/control bytes are explicitly escaped without guessing encoding. Container Result/contract behavior remains; UI separately bounds complete receive-image size.

`include/vinc/message/detail/preview.hpp` 是指定供框架 UI 的唯讀接線，不是新的傳輸格式。
呼叫端必須提供已驗證、自己持有且不被並行修改的本機生成訊息，不傳入活躍 shared write loan。
透過既有 Ribosome `ribosome_visit_fields` 的編譯期型別資訊走訪，支援純量、字串、陣列與巢狀 record，
不建立動態 runtime schema registry，也不改變生成器、schema hash 或 payload 映像。
呼叫端明確給文字 bytes、每陣列元素數、總走訪節點、深度及字串 bytes 預算，裁切標示省略與陣列原長度。
文字為人讀取的診斷，不保證 JSON／可反序列化；不猜字串編碼，非 ASCII／控制 bytes 明確 escape。
正常容器存取與 Result／contract violation 契約不變；UI 接收完整映像的上限另由 UI 管理。

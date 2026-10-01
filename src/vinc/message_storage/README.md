# 訊息儲存與 MessageArray

本元件為 Ribosome 生成的訊息 class 提供儲存、陣列欄位與可搬移格式。
正常入口是 `vinc::make_message<生成的訊息型別>()`；`MessageArray` 是訊息裡的陣列容器。
獨立使用容器不會取得發布資格。使用 C++23，目前限定 64-bit 平台。

```cpp
#include <vinc_messages/Frame.hpp> // 示意；Frame 定義在測試 fixtures

auto msg = vinc::make_message<vinc::Frame>();
msg->data().sequence = 42;
auto result = msg->data().detections.resize(2);
if (!result) { /* 處理 result.error().code */ }
msg->data().detections[0].data().id = 7;

auto copied = msg->data().detections;  // 深複製訊息內容，獨立本機儲存
auto& ref = msg->data().detections;    // 原訊息的陣列欄位
auto* ptr = &msg->data().detections;   // 指向原欄位
```

## 三條資料路徑

| 路徑 | 已實作的訊息儲存行為 |
| --- | --- |
| 接收共享訊息 | 生成類別的 `from_bytes()` 複製完整映像一次，再驗證副本並建立本機欄位存取介面；解除來源映射後可繼續讀寫 |
| 借用共享訊息後填寫 | 私有 `FrameworkAccess::borrow_message<T>()` 綁定固定 byte 上限；各欄位直接寫入共享資料；`seal()` 驗證並撤銷寫入，不複製或搬移 payload |
| 提交本機訊息 | 私有 `FrameworkAccess::push()` 先計算緊密布局與容量，再將有效資料直接寫到目的共享區；超限保留來源與目的區原內容 |

**整合邊界：** 這些借用、提交入口維持在私有 `src/framework_access.hpp`，由
[Bus 的 Publisher／Subscriber](../bus/README.md) 接上共享 Ring、slot 同步與失聯回收。
Bus 接收端只複製完整映像一次，透過 `adopt_record` 接管該本機 buffer 後再驗證及建構 class。
一般使用者從 Module 的 `bus()` 取得 Publisher／Subscriber，不直接建立共享訊息。

框架傳入的 owner 必須保留映射及 slot lease，並保證借用時獨佔寫入。
接收複製期間必須保護來源，完成後才能讓發布者重用 slot。
`seal()` 回傳的 Snapshot 持有 owner；它不執行跨程序 atomic 或發布通知。
借用訊息銷毀、明確 revoke 或成功 seal，都會結束可寫生命週期。

## 訊息與欄位的所有權

- 每個生成訊息的所有純量、子訊息、陣列及字串，綁定到同一份儲存。
  C++ class／虛擬函式表／存取 wrapper 都位於本機，未放入共享映像。
- 複製訊息 class、`Data_t`、陣列或自訂訊息元素，會取得獨立本機內容。
  即使來源是共享借用，副本在原借用結束後仍然有效。
- 對已存在的欄位賦值／`assign()`，會寫入目標所屬儲存，保留目標 binding 與容量限制。
  賦值不會透過更換儲存繞過固定上限。`Data_t` 和訊息的賦值也遵守此規則。
- 移動獨立物件可移交其儲存；從訊息內的欄位移出會複製到本機，保留父訊息結構。
  對既有欄位的 move assignment 同樣保留目標 binding，不保證 O(1)。
- `auto&`／指標不複製資料；使用期限仍受所指物件、陣列元素及借用生命週期約束。
  已刪除子樹的存取介面會失效；不可保留並解參照已被銷毀的 C++ 物件指標。
- 訊息 metadata（topic、發布者、時間、topic count、Publisher instance）屬於 `BaseMessage`；
  payload 格式不攜帶這些路由資料，由 Bus 在接收端補上。子訊息不具有獨立發布通道。

## 支援型別與操作

訊息陣列支援固定寬度整數、IEEE 754 float／double、bool、生成的訊息 class、
`MessageString` 與巢狀 `MessageArray`。布局規則由 Ribosome 生成，並遞迴驗證。
bool 每個值使用一個 byte（0／1），`std::vector<bool>` 匯入逐元素編碼。

`MessageArray` 提供大小／容量查詢、resize、reserve、clear、shrink_to_fit、
push_back、emplace_back、pop_back、set、索引、迭代與 vector 匯入／複製回 vector。
`resize` 的新增數值為零，新增訊息的欄位初始化為零或空內容；`.msg` 常數不占 payload。
`at()` 對訊息元素回傳獨立副本；`[]` 對訊息元素回傳綁定欄位的參照。

一般 C++ 型別也可以獨立使用：

```cpp
vinc::MessageArray<std::string> names;
vinc::MessageArray<std::unique_ptr<MyClass>> objects;
```

這類本機物件使用標準容器管理 C++ 建構／移動／析構，不允許放進訊息映像。
複製行為取決於元素：指標只複製指標，move-only 元素的容器不能作元素複製。
任意使用者型別拋出的例外遵循該型別與標準容器的行為。

### 存取介面與 vector 的差異

- 訊息純量欄位使用 `Scalar<T>`，支援賦值、取值與隱式轉換；`get()` 可取得明確的原生 T。
  複製 Scalar 會取出獨立本機值。它不是原生 `T&`，需要精確 T 的模板呼叫可使用 `get()`。
- 訊息純量陣列的非 const 索引是 proxy；const 索引取值。未提供原生 `T* data()`。
  `auto element = array[i]` 可能取得 proxy；要明確複製純量，可用 `at()` 或轉成 T。
- 訊息元素索引使用本機 wrapper cache。子欄位搬移不改變所屬元素 handle。
  改變該陣列本身的大小、容量或替換內容，會使其既有迭代器／proxy 失效。
  增長元素內部的陣列不會使外層迭代器失效。
- `MessageArrayView<生成訊息>` 從獨立陣列 Snapshot 讀取元素時，回傳本機訊息副本。
  接收整筆訊息應使用生成類別的 `from_bytes()`，不需要逐元素反序列化。
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

修改 API 多以 `std::expected` 回報容量、配置、大小溢位等錯誤；賦值運算子、
索引與無效存取以 `AccessError` 回報。建構與 wrapper 配置仍可能拋出配置例外。
本機與共享 facade 的操作需由呼叫端序列化，包括會建立元素 wrapper 的 const 存取。
已擁有的 immutable Snapshot 位元組可並行讀取。本元件不宣稱 lock-free。

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
早期格式會隨設計演進，尚未承諾穩定 ABI；64-bit schema hash 用於相容性識別，不是驗證來源真實性的安全機制。

## 驗證入口

在 x64 MSVC developer 環境，從 `src`：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --test-dir ../build/windows-msvc-debug --output-on-failure
```

`message.storage` 保留原有陣列／容量／損壞映像測試；`message.generated` 使用實際 Ribosome
生成的 class，驗證工廠、巢狀欄位、深複製、借用、提交、失敗原子性及 OSAL 跨程序傳遞
2 MiB payload。接收程序在不同位址映射，複製一次後解除共享映射，再讀寫本機訊息。
Windows Debug／Release 為本次驗證平台；Linux／macOS 尚待實機驗證。

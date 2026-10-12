# Ribosome 訊息產生器

Ribosome 將 `.msg` 編譯成可直接建立、複製與操作的 C++ class。
每個生成 class 繼承 `GeneratedMessage<T>`／`BaseMessage`，保留 `data()`、
`type_hash()`、`msg_name_str()`、`to_string()` 與序列化介面。
陣列欄位使用 `MessageArray`，整筆訊息共同管理其儲存。

```text
# Detection.msg
uint32_t id;
float confidence;
float3 position;
uint8_t[] extra;
string label;
```

```text
# Frame.msg
uint64_t sequence;
Detection[] detections;
Detection[][] groups;
uint32_t protocol = 7;
```

```cpp
#include <vinc_messages/Frame.hpp>

auto msg = vinc::make_message<vinc::Frame>();
msg->data().sequence = 42;
auto result = msg->data().detections.resize(2);
if (result) {
    msg->data().detections[0].data().id = 10;
    auto named = msg->data().detections[0].data().label.assign("person");
    if (!named) {
        return;
    }
}
auto independent = msg->data().detections;
```

連結 `vinc_messages` 即取得生成依賴、標頭路徑及儲存 library。
Frame／Detection 是以上示例與測試 fixture，沒有加入專案的正式 `vinc_messages/messages`。
完整的容量、借用與複製契約見 [訊息儲存文件](../components/message_storage/README.md)。

## 語法與型別

- 一行一個宣告，分號可省略。支援 `//`、`#` 註解，字串常數內的註解字元不會截斷內容。
- `T[] name` 或 `T name[]` 表示可變陣列；可用多層 `[][]`。
  方括號需集中在型別或欄位名稱一側，不接受混用或固定長度 `[N]`。
- `T name = value` **保持既有語意：class 常數**，不占 payload，也不是欄位預設值。
  支援數值、bool 及字串常數；不接受陣列／訊息常數。
- 型別名稱與欄位名稱使用 ASCII 字母開頭，後接字母、數字、底線。
  拒絕 C++ 關鍵字、生成介面保留名、雙底線與 `ribosome_` 前綴。
- 支援空訊息。拒絕未知型別、重複欄位、生成符號衝突、大小寫檔名衝突、
  直接或間接循環引用（包括經過陣列的循環）。
- 每個定義檔上限 1 MiB、每行 4096 bytes、每個訊息 4096 個欄位／常數，
  陣列維度上限 64。相依深度以根訊息為 0、每次引用子訊息加 1，最長路徑上限 64，
  包含經過陣列的引用，不受檔名排序影響。另檢查 Storage 初始化深度：根 record 為 0，
  每個 payload 欄位加 1；非陣列子訊息繼續遞迴，空陣列／字串停止，常數不占 payload。
  此深度上限亦為 64；錯誤附來源檔及行號。

固定寬度 `int8_t`／`uint8_t` 至 `int64_t`／`uint64_t` 生成 `std::` 版本。
`int8`、`uint8` 等為相同型別的輸入別名。float／double 使用 IEEE 754 32／64 bits，bool 為單 byte。

| `.msg` 舊別名 | 正規化後的訊息型別 |
| --- | --- |
| char | int8_t |
| short | int16_t |
| int | int32_t |
| long | int64_t |
| size_t | uint64_t |

這是訊息語言的固定規則，**不依 host 的 `sizeof(long)` 改變**。
原生 C++ 數值可透過一般轉換指定給欄位，呼叫端仍須注意自己的縮窄轉換。
string 生成 `MessageString`，以 byte 序列表示，沒有把 `std::string` 的內部指標放入映像。
目前不強制 UTF-8 驗證；嵌入零 byte 的執行期字串可用明確長度指定。

浮點常數以宣告的 float／double 解析，拒絕超出可表示範圍（含解析下溢）的輸入，
再輸出具有足夠往返精度的科學記號 literal；保留負零與既有 `nan` 特例。
前導零沒有八進位含義。字串常數支援 `\n`、`\r`、`\t`、`\0`、`\"`、`\\`；
`\0` 只表示一個零 byte，不吞入後面的數字。生成器使用逐 byte escape 與明確長度
建立 string_view，保留內嵌零、UTF-8 bytes 及 `${...}` 文字。

## 布局、識別與相容性

生成 schema 包含欄位順序、正規化型別、陣列維度與巢狀訊息識別。
SHA-256 輸入採長度分隔，包含格式版本、訊息名、欄位、常數及相依訊息 hash，
取前 64 bits 作為識別；同次生成檢查碰撞。
註解及型別別名不影響 hash，子訊息變更會影響所有引用它的父訊息。
這是相容性識別，不是密碼學認證；不承諾不同建置間絕對無 64-bit 碰撞。

新版使用 [v2 訊息映像](../components/message_storage/README.md)，與舊逐欄位 raw 序列化及 v1 陣列映像不相容。
`serialize()` 與 `deserialize()` 均回傳 `Result<void>`；前者成功時附加完整映像到 vector，
後者失敗保留目標 payload、binding 與 offset，並保留具體錯誤分類。
無 offset 版本拒絕尾端多餘資料；帶 offset 版本支援串接訊息，成功才推進 offset。
錯誤分類與原生布局 helper 的移除見 [Message Storage 解碼介面收尾](../components/message_storage/README.md#解碼介面收尾2026-10-05)。
正常接收路徑用 `T::from_bytes()` 複製一次，`deserialize()` 為保留目標 binding
還需要內容替換，不是單次複製的接收快速路徑。

純量欄位目前為可綁定的 `Scalar<T>`，不是原生 T 成員；一般賦值與轉換可用，
需要精確原生型別的模板呼叫使用 `.get()`。訊息類別／Data_t 已不保證 aggregate 初始化或舊的記憶體布局。

## 容器契約與生成代理（2026-10-04）

- `MessageArray::operator[]` 不配置資源；巢狀元素回傳輕量 proxy value，record 的
  `data()` 建立 `Data_tRef<ReadOnly>` 欄位集合，不建立 owning message／wrapper cache。
- 使用 `auto element = array[i]` 或 `for (auto element : array)`；複製 proxy 仍借用原資料。
  const／Snapshot 路徑產生唯讀 proxy，複製後不會取得寫入權限。
- `at()` 回傳 `Result<T>`，訊息／巢狀容器為獨立本機副本；`set()` 與 `assign()` 回傳 Result。
- owning class、Data_t、MessageString 及 MessageArray 的整筆 copy／move assignment 已刪除；
  owning class 的複製／移動建構保留；`Data_t` 的 rvalue 仍由 copy constructor
  建立獨立本機內容，不承諾移交 backing。純量欄位及純量元素 `=` 保留。
- 新增生成名稱 `DataRef`、`ReadOnly` 及 `<訊息名>Data_tRef` 的衝突檢查；
  與既有名稱檢查同樣在生成前拒絕，避免產生無法編譯的 C++。
- 越界 `[]`、失效 handle／iterator 與撤銷借用後存取走 common contract handler，沒有 AccessError。
  容量等預期失敗用 Result；一般 C++ 配置／使用者例外政策維持既有邊界。

生成 schema hash、v2 映像及 `.msg` 語法未改；消費端需重新生成標頭並適配上述 API。
`message.access` 使用實際生成的 class 驗證首次存取零配置、唯讀、代理與獨立副本。

## 生成流程

唯一模板是 `data/message_sample.txt`，必要標籤為 `DO_INCLUDE_DEPENDENT_MESSAGES`
與 `DO_MESSAGE_CLASS`。舊 `include/ribosome/message_sample.hpp` 只留下導向說明。
原模板的兩個必要標籤各需出現一次；未知、重複、未結束或缺少標籤立即失敗。
驗證在插入生成內容之前完成，不重新解析訊息常數中的文字。

預設輸入為 `vinc_messages/messages`，輸出為本次建置目錄的 `generated/include/vinc_messages`。
路徑由 CMake 設定：預設值使用明確的 C++ UTF-8 字面值，Windows CLI 使用原生
UTF-16 參數，POSIX CLI 保留原生 path bytes。Windows 路徑不經 ANSI code page，
生成器自有的路徑診斷輸出 UTF-8；不新增 OSAL runtime 相依。也支援測試／工具使用：

```text
ribosome --input PATH --output PATH --template PATH
```

輸出資料夾應專供生成檔使用，每次生成先移除舊輸出，全部成功才從 sibling staging
資料夾換入。拒絕輸出資料夾為 symlink，以及輸出包含輸入或模板的情況。
存在 staging 代表另一次生成或異常中止，會拒絕繼續；確認沒有產生器執行後再清理。
共用同一個輸出資料夾的並行生成不受支援。

### 程式碼閱讀入口

- `Ribosome::load()`：排序輸入、檢查訊息名稱／檔案大小；`parse_fields()` 讀取宣告，
  `parse_declaration()` 處理欄位名稱、陣列維度與型別正規化。
- `validate_constant()`：區分字串、bool、浮點與整數；數值範圍檢查由
  `floating_constant()`／`integer_constant()` 負責。
- `Ribosome::resolve()`：使用 `VisitState` 呈現相依遍歷狀態；保留原本逐欄位、
  長度前綴、子訊息 hash 的輸入順序；快取最長相依與初始化深度。
- `Ribosome::generate()`：依序呼叫 `emit_*` helper，組合相依標頭、schema、Data_t、
  訊息 metadata、建構／指派、資料存取與文字表示。生成器直接產出展開後的 C++ 函式。
- `make_ligand()`：驗證輸出路徑、取得 staging、載入模板、載入訊息／解析相依、
  寫入整組標頭，最後將 staging 換入正式輸出。`StagingDirectory` 僅清理本次成功
  取得的目錄；模板或訊息出錯時仍沿用清除舊輸出的政策。
- `sha256.hpp`：分段輸入、padding、message schedule、compression 與 digest 輸出。
  `a`～`h` 保留 SHA-256 慣用工作字名稱；其他狀態與容量使用具名值。

編譯工具的輸入大小／欄位數／深度上限集中具名於 `ribosome.cpp`，數值維持原樣；
這些是建置時 `.msg` 的驗證限制，與執行期訊息容量及 Param 分開。
SHA-256 的區塊大小、輪數、位移與常數表仍由雜湊實作維護。
本輪保留既有 CLI、公開 API、錯誤回傳／例外政策與 schema 格式。

## 測試

- `ribosome.hash`：標準 SHA-256 向量、分段更新與重複使用，另含 55／56／63／64／65-byte
  padding 邊界及內含零 byte／高位元 byte 的輸入。
- `ribosome.schema`：語法、別名、相依 hash、循環、常數範圍、錯誤後輸出清理與復原。
- `ribosome.generated`：實際編譯常數／名稱回歸 fixture，以 static_assert 驗證生成值，
  並執行負零、NaN、欄位操作、assign 與移動建構。
- `message.generated`：實際生成 class 的功能與跨程序資料驗證。
- `tests/ribosome_generation.cmake`：獨立來源副本中的建置失敗、復原、逐一標頭編譯與正式模板的巢狀訊息實例化。

Windows Debug／Release 有原生驗證；Linux／macOS 尚未實機驗證。

### 可讀性整理驗證（2026-10-04）

- Windows x64／MSVC：Debug、Release 完整建置成功，包含生成標頭使用端及 SDK 標頭編譯。
- `ribosome.hash`、`ribosome.schema`、`message.generated`、`message_bus.module`、
  `sdk.consumer`、`message_bus.shell`：Debug 6/6 通過（12.11 秒），Release 6/6 通過（6.85 秒）。
- 比對整理前後的正式訊息與 fixture：13 份生成標頭在統一排版、補齊既有單行 if 的
  大括號並排除註解後，C++ token 相同；11 個 schema hash 全數保持相同。
- `tests/ribosome_generation.cmake`：獨立 Debug 來源副本的 13 個步驟全部通過，包含
  逐一標頭編譯、重複生成、缺失／空模板、缺失／無效輸入、失敗清理及復原、完整建置、
  正式模板的巢狀訊息編譯與還原。
- Linux／macOS 當輪未驗證；相依深度疑慮當時未修正，現已於下方 2026-10-05 重審工作處理。

## 重審修正（2026-10-05）

七項已重現問題已修正：

1. **浮點值：** `010`／`08` 分別生成十進位浮點值 10／8，消除原 C++ 八進位解讀；
   直接依宣告型別解析、正規化輸出，測試精度、最大值、次正規最小值、負零及範圍拒絕。
2. **字串長度：** `"a\0b"` 保留三個 bytes，`"\0127"` 保留零 byte 與三個數字；
   escape 解析與 C++ 輸出分開，避免標準庫的 C 字串長度及 C++ escape 規則改變內容。
3. **輸出名稱：** 在大小寫正規化的輸出名稱集合中預留 `all_messages`，拒絕
   `ALL_MESSAGES.msg`／`All_Messages.msg`，避免 Windows 覆蓋 class 標頭後仍回報成功。
4. **C++ 符號：** 訊息名稱另檢查其共用的 vinc 命名空間；拒絕 `Result`、`Error` 等
   既有型別／符號。內部基底別名改用已保留前綴 `ribosome_Base`，`Base` 常數仍可使用；
   Result 回傳型別與自訂訊息欄位型別使用完整限定名稱。`Result`、`b`、`access` 欄位及
   `Point Point` 均由實際生成標頭編譯／執行驗證。
5. **深度：** 保存最長相依鏈與初始化深度，不再僅依 DFS 當下堆疊判斷。
   兩種檔名／遍歷方向皆測試 70 節點拒絕、65 個空訊息節點接受、66 個拒絕；
   有純量葉欄位時 64 個訊息節點接受、65 個拒絕，對齊 Storage 的初始配置計數。
6. **模板：** 僅驗證原模板，生成內容不當成待解析標籤；常數內的 `${VALUE}`、
   `${DO_MESSAGE_CLASS}` 與 `${DO_INCLUDE_DEPENDENT_MESSAGES}` 保留原文字。
7. **Unicode 路徑：** Windows 改用 wmain，CMake 預設值改用 u8 字面值；staging 名稱
   直接在 native path 上附加後綴，診斷不經 `.string()` 的 ANSI 轉換。
   CMake 測試擷取 generator 診斷亦明確指定 UTF-8。

### 相容性

已獲使用者同意：常數正規化會影響參與 hash 的文字，含相關常數的訊息及父訊息
可能改變 schema hash。所有相連程式須重新生成並建置；舊 hash 依既有契約拒絕，
不提供相容模式。訊息映像仍為 v2；沒有新增手動版本欄位或 runtime error catalog。
此次比對專案六個正式 `.msg` 的生成 hash 均未變；不代表所有使用者 schema 都不變。

### 驗證

Windows x64／MSVC 19.51.36257、SDK 10.0.26100、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 完整建置成功，包含 SDK 標頭獨立編譯與 Vinc／範例。
三個 `ribosome.*`、四個 `message.*`、六個 `message_bus.*` 及 `sdk.consumer`
相關 CTest 各 **14/14** 通過。`message.access` 的既有 Debug 配置注入限制維持原狀，
詳見 Message Storage README。

`ribosome.schema` 使用真實 generator 檢查失敗清理／復原、常數正規化的父 hash、
深度邊界及 Unicode 的輸入／輸出／模板路徑與錯誤檔名；`ribosome.generated`
編譯及執行真正的生成 class。另於獨立的中文＋emoji 來源／建置路徑，建置僅含
Ribosome 的 Release target，使用含 Unicode 的 CMake 預設輸入／輸出路徑，
不帶 CLI 參數執行成功。此獨立驗證未引入 runtime library 相依。

未重跑全專案所有 CTest 或 `tests/ribosome_generation.cmake` 的完整來源副本流程；
Linux／macOS、MinGW 尚未實機驗證。新增案例已納入元件正式測試。

## 編譯期訊息清單（2026-10-12）

生成目錄增加 message_types.hpp，提供 `vinc::message::visit_types(visitor)`；
對每個生成類別呼叫 visitor 的 template operator()<T>()。
Bag Recorder 用它建立已知型別的 typed subscription，不引入 Bag 相依或 runtime registry。
message_types 與 all_messages 同為保留輸出名稱；schema hash／image layout 不因清單而改變。

Python 綁定亦使用此清單；生成 class 提供 `ribosome_visit_fields(visitor)` 與
`ribosome_visit_constants(visitor)`，分別交付編譯期欄位型別／索引和常數名稱／值。
這些是生成器的接線介面，不加入 Python runtime 相依，也不改變 hash 或映像布局。
Python 的界線、轉型及借用生命週期見 [Python Module](../module_system/python/README.md)。

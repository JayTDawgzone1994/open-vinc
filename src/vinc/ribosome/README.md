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
    msg->data().detections[0].data().label = "person";
}
auto independent = msg->data().detections;
```

連結 `vinc_messages` 即取得生成依賴、標頭路徑及儲存 library。
Frame／Detection 是以上示例與測試 fixture，沒有加入專案的正式 `src/messages`。
完整的容量、借用與複製契約見 [訊息儲存文件](../message_storage/README.md)。

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
  陣列維度與訊息相依深度上限各 64；錯誤附來源檔及行號。

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

## 布局、識別與相容性

生成 schema 包含欄位順序、正規化型別、陣列維度與巢狀訊息識別。
SHA-256 輸入採長度分隔，包含格式版本、訊息名、欄位、常數及相依訊息 hash，
取前 64 bits 作為識別；同次生成檢查碰撞。
註解及型別別名不影響 hash，子訊息變更會影響所有引用它的父訊息。
這是相容性識別，不是密碼學認證；不承諾不同建置間絕對無 64-bit 碰撞。

新版使用 [v2 訊息映像](../message_storage/README.md)，與舊逐欄位 raw 序列化及 v1 陣列映像不相容。
`serialize()` 附加完整映像到 vector；`deserialize()` 成功=0，失敗=-1，
失敗保留目標 payload 與 offset。無 offset 版本拒絕尾端多餘資料；帶 offset 版本支援串接訊息。
正常接收路徑用 `T::from_bytes()` 複製一次，legacy `deserialize()` 為保留目標 binding
還需要內容替換，不是單次複製的接收快速路徑。

純量欄位目前為可綁定的 `Scalar<T>`，不是原生 T 成員；一般賦值與轉換可用，
需要精確原生型別的模板呼叫使用 `.get()`。訊息類別／Data_t 已不保證 aggregate 初始化或舊的記憶體布局。

## 生成流程

唯一模板是 `data/message_sample.txt`，必要標籤為 `DO_INCLUDE_DEPENDENT_MESSAGES`
與 `DO_MESSAGE_CLASS`。舊 `include/ribosome/message_sample.hpp` 只留下導向說明。
未知或缺少標籤立即失敗，避免生成看似成功但缺少 class 的檔案。

預設路徑由 CMake 設定；也支援測試／工具使用：

```text
ribosome --input PATH --output PATH --template PATH
```

輸出資料夾應專供生成檔使用，每次生成先移除舊輸出，全部成功才從 sibling staging
資料夾換入。拒絕輸出資料夾為 symlink，以及輸出包含輸入或模板的情況。
存在 staging 代表另一次生成或異常中止，會拒絕繼續；確認沒有產生器執行後再清理。
共用同一個輸出資料夾的並行生成不受支援。

## 測試

- `ribosome.hash`：標準 SHA-256 向量、分段更新與重複使用。
- `ribosome.schema`：語法、別名、相依 hash、循環、常數範圍、錯誤後輸出清理與復原。
- `message.generated`：實際生成 class 的功能與跨程序資料驗證。
- `tests/ribosome_generation.cmake`：獨立來源副本中的建置失敗、復原、逐一標頭編譯與正式模板的巢狀訊息實例化。

Windows Debug／Release 有原生驗證；Linux／macOS 尚未實機驗證。

# Bag Recorder

第一版提供 **Recorder Module、SQLite 錄製格式及 metadata 查詢**。
Player 已決定另作 Module，播放功能不屬於本輪；沒有動態載入訊息或執行訊息描述的直譯器。

## 使用

`VINC_BUILD_APPS=ON` 會建置並登記 `bagRecorder` ModuleType，預設 Module 名稱是 `recorder`。
它是由 ModuleHost 啟動的 DLL／共享 library，不在中央程序內執行。

在 Vinc Shell 或 Tcl 腳本中：

```tcl
bagRecorder record session01 --all
# 等待 Started recorder 後：
recorder status
recorder finish
# finish 非同步；status 的 finished=1 才表示 worker 已結束。
recorder info session01
```

只錄指定 topic：

```tcl
bagRecorder --name cameraRecorder record camera01 --topic camera/frames --topic camera/status
cameraRecorder status
cameraRecorder finish
```

也可以只啟動空閒 Module，再執行 `recorder record DIR --all`。
上一份 worker 完成後可開始新的錄製。目錄必須不存在、父目錄必須已存在；不覆寫、
不追加既有錄製、不自動刪除舊 Bag。相對路徑依 ModuleHost 的工作目錄解析，建議部署使用絕對路徑。
執行 `bagRecorder info DIR` 可只查資訊；Module 仍保持運作，之後可再下 record 指令。

record 選項：

- `--all`，或一個以上 `--topic NAME`，兩者互斥。
- `--database-bytes N`：主資料庫大小上限，預設 16 GiB，最低 4 MiB；交易暫存空間另計。
- `--queue-bytes N`：待寫入資料預算，預設 256 MiB。
- `--message-bytes N`：完整共享訊息映像上限，預設 129 MiB，包含布局及對齊，容納典型 128 MiB payload。
  不能超過編譯期上限或 queue 預算。框架按完整映像計算，並非只算某個 array 長度。

## 元件與相依

- `vinc::bag`：SQLite 儲存、metadata 查詢及 bounded Recorder worker；SQLite 為私有相依。
- `module/recorder_module.cpp`：一般 BaseModule Manual 模式，使用繼承 Context、pumpOnce 及 typed subscribe。
- Ribosome 額外產生 `message_types.hpp` 的 `vinc::message::visit_types()`。這是建置時確定的 C++ 型別分派，
  不增加 runtime schema registry，也不將 Bag 相依加進生成訊息。
- Recorder 依 `vinc::messages` 編譯已知訊息；新增 `.msg` 後重新配置／建置 Recorder。
  SDK 及其他 Module 不需連結 SQLite。
- 所有 Bag 預期失敗使用 common Result／Error；ErrorDomain::Bag=12。SQLite extended result code
  保存於 NativeErrorSource::Sqlite=11，錯誤表不相依 SQLite runtime。

## Topic 探索與資料路徑

Context `list_topics(after)` 經控制 Socket 取得有發布者的 topic、schema hash、發布者數。
每頁上限 32 筆，依名稱排序；每頁一致，跨頁沒有全域原子快照。Recorder 預設每 500 ms 重查。
不增加中央 payload 轉送或額外探索執行緒。

已知 hash 使用既有 `subscribe<T>()`，從 NewOnly 開始；框架驗證布局並複製到程序內，
釋放共享 Reader 占用後才呼叫 callback。Callback 交付訊息所有權到 queue，worker 才建立
canonical Snapshot 並送 SQLite。沒有直接操作 RingBuffer 的錄製捷徑。

`SubscriberOptions::max_message_bytes` 在 Bus 配置接收 buffer 前檢查。Callback 遇到過大訊息會推進
該來源游標並增加 `SubscriberStats::oversized_messages`；get_last 則回 CapacityExceeded，不推進游標。
預設 0 保留既有無額外接收上限行為。

所有來源均會錄製，Publisher 重啟後保留新 Publisher ID。沒有活動 Publisher 的 topic
會關閉訂閱，下輪可重新訂閱。短於探索週期的 topic、訂閱建立前已發布的內容、發布者退出時已退役
的未讀資料可能沒有錄到；`--all` 不代表無損或全時段覆蓋。

未知 hash 會保存 topic／hash 及 supported=false，輸出 `unsupported type`，略過 payload；
其他 topic 繼續。不會把「全部已錄成功」套用到這些項目。

## 有界 queue、壓力與狀態

預設 queue 限制 256 MiB／4,096 筆，writer 已取出的當批資料仍占預算，直到交易提交且本機訊息釋放。
滿載丟棄新到的訊息（DropNewest），不等待磁碟，也不保留共享 Reader lease。
短 mutex 只保護 queue／計數／狀態；序列化與 SQLite I/O 都在 mutex 外。

此預算是待寫入訊息的 allocated image bytes，不是整個 process RSS。
接收中訊息、儲存結構索引、訊息 wrapper、當前 Snapshot、SQLite cache（預設 8 MiB）及 OS cache 另計。
單筆接收與 Snapshot 有 message_bytes 上限，metadata 有 topic／來源數及字串長度限制。

計數分開：

- received：進入 Recorder enqueue 的訊息。
- written：已提交 SQLite 的訊息。
- queue_dropped：byte／筆數預算滿而拒收。
- oversized：Bus 接收限制或 Recorder 單筆限制而略過。
- bus_lost：Bus 回報的 ring 覆蓋遺失；不宣稱涵蓋訂閱前或 Publisher 退出時所有無法量測的遺失。
- unsupported_topics：本次探索過但不支援的 topic/hash 組合，個別狀態也持久保存。

批次預設最多 256 筆、16 MiB 或 100 ms；單筆超過 16 MiB 時獨占一批。
磁碟慢時不保證 100 ms 持久化；沒有新資料／metadata／計數時不進行空交易。

## SQLite 格式及容量

一份 Bag 是獨立目錄，內含 `recording.sqlite` 及 SQLite 交易所需檔案。
原子建立目錄避免兩個 Recorder 共用同一路徑。建立失敗後的部分目錄保留供診斷，不能當完整 Bag。

格式 v2 使用 application_id `0x56424147`／user_version=2；舊版與未知版本回 InvalidFormat，不自動轉換或刪除：

- recording：開始 UTC nanoseconds（uint64，20 位 TEXT）、完成旗標、統計。
- topics：topic、訊息類別名稱、schema hash、錄製時是否支援。
- messages：topic/hash、Publisher 名稱與 ID、原始 topic count、發布 `source_utc_ns`、
  發布 `source_monotonic_ns`、接收 `receive_monotonic_ns`、image_version=2 及 canonical BLOB。
  Recorder 保存收到的兩個發布時間，不重新取樣覆蓋它們。
- `TopicSummary.first_timestamp/last_timestamp` 為已錄資料的最小／最大 Unix nanoseconds；
  系統校時時不一定對應第一／最後插入的訊息。Shell info 顯示 UTC 日期；播放排序仍需使用單調時間。
- `Info.start_utc_ns` 改為 uint64。建立時系統時間無法表示回 Bag TimestampUnavailable，保留 Common 原因。
- 完整 uint64 metadata 使用固定 20 位十進位 TEXT，避免 SQLite signed INTEGER 截斷；payload 與 metadata 分開。

採 DELETE rollback journal、synchronous=EXTRA、批次交易；未關閉 SQLite 同步保障。
`Options::database_bytes`／`--database-bytes` 只限制主資料庫，包含訊息、metadata、索引及頁面成本。
預設 `config::bag_database_bytes` 為 16 GiB，最低 `config::bag_min_database_bytes` 為 4 MiB。
不再扣除 journal 預留或除以四；最大頁數為 `floor(database_bytes / 4096)`，非整頁設定向下取整。
目前 SQLite 上限是 4,294,967,294 頁；超過其與 4,096 bytes 的乘積便回 InvalidArgument，
不默默縮小，且在建立目錄前拒絕。建立時會核對 SQLite 實際套用的頁數上限。
舊的 `capacity_bytes`／`--capacity-bytes` 名稱已移除，不提供相容別名。

這不是目錄總容量或檔案系統 block quota。SQLite journal 另外占空間，錄製期間總占用可能
超過指定值，也不預先向 OS 保留磁碟空間。Recorder 以本次連線執行限額，不能限制外部工具改寫檔案。

不能再容納本批交易、磁碟不足或永久 I/O 錯誤時，停止該次錄製並回報，不輪替、不覆蓋舊資料。
先前已提交交易保留；當批交易整批回滾，連同 queue 中未提交資料捨棄，不拆批重試填滿剩餘空間。
因此可能尚未填滿主資料庫上限就停止，只有已提交資料才計入 written；completed 保持 false。
SQLite FULL 可能代表頁數上限或實際磁碟不足，回報 CapacityExceeded 並保留 SQLite 原生碼，
不僅憑 FULL 就聲稱已確定是哪一種。其他 I/O 故障維持 DatabaseFailed。
Recorder Module 保持可操作，可用新目錄再開始錄製；不因這項錄製故障要求中央退出。

短暫資料庫鎖競爭等待最多 2,000 ms，由 config 控制，只阻塞資料庫所屬執行緒。
超過仍回報 DatabaseFailed；請避免外部工具長時間鎖住正在錄製的檔案。

## 停止、崩潰與查詢

finish 先關閉訂閱、停止入列，再由 worker 排空。Module 保持 alive，status 可查收尾進度。
正常 Module stop 會等待 worker，內建 Recorder 的 stop_timeout 預設 30 秒（config）；超時仍依
既有 ModuleHost 政策終止程序，不延伸全域關閉期限，也不改其他 Module 的 stop timeout。
不保證中央同時停止所有 Publisher 時能收齊它們的最後一筆資料。

完成旗標表示最終成功交易已提交；**不表示沒有丟訊息、沒有未知型別或完整涵蓋所有 Publisher**。
磁碟故障、callback／探索故障保留 incomplete；中央及其他 Module 不因 Bag 故障而退出。
突然終止可能丟失 queue 與未提交交易，統計以最後成功提交的版本為準。

info 不需認識訊息布局即可讀 topic、名稱/hash、筆數、時間與原 Publisher metadata；
本機沒有 hash 時顯示 unknown type，同名不同 hash 顯示 schema mismatch。它不解碼 payload。
查詢採唯讀、關閉不可信 schema 功能並限制 SQLite VM 執行預算；超大型查詢可回錯誤。

崩潰留下 hot rollback journal 時，SQLite 復原需要寫入權限。唯讀 info 可能回報 read-only recovery 錯誤，
須先用 SQLite 正常讀寫連線完成 rollback recovery；第一版不提供自動修復命令。
移動／備份未正常完成的 Bag 時要保留整個目錄，不能只複製主資料庫。

## 驗證與待處理事項

2026-10-12：Windows x64、MSVC 19.51.36257.0、CMake 4.3.2。
Recorder 初版 Debug／Release 全專案 CTest 均為 **70/70 通過**；下方容量政策修正另行驗證。

本輪新增／擴充的驗證包括：

- 1、2、4、8、16、32、64、128 MiB payload 與巢狀欄位的 SQLite 寫入／解碼比對。
- queue byte／筆數上限、worker 已取出資料仍占預算、DropNewest 及非同步排空。
- 晚出現 topic、多發布者、發布者重啟、過大訊息略過、未知型別與 schema mismatch。
- SQLite FULL、外部持鎖超時、Recorder 故障後再次錄製，中央／發布者保持可操作。
- 子程序突然退出留下 hot journal，經 SQLite 復原後僅保留已提交交易。
- UTF-8 中文／emoji 路徑、Module 正常停止排空，以及真正 Shell／Tcl 啟動命令。

SQLite FULL 由 page quota 製造；未將測試磁碟實際寫滿。上述測試不構成錄製吞吐量 benchmark。

- 跨平台實作使用 SQLite／標準 filesystem／既有 OSAL；Linux、macOS 尚未實機驗證。
- 已確認邊界：只限制主資料庫；交易暫存空間另計，不提供目錄總容量硬限制。
- 已知限制：唯讀查詢不自動修復 hot journal；未做 Bag 專用復原命令。
- 尚未實作：Player、播放速度／重播時序、錄製切檔、動態 schema 擴充。
- 驗證不可涵蓋真實磁碟斷電、硬體損壞或所有檔案系統同步行為；SQLite 交易測試不是硬體持久性的保證。

### 主資料庫容量政策修正

已確認取消四分之一預留、整批失敗不拆批重試。錄製盡力收錄，不保證與發布者相同 Hz，
保留原始時間戳，不補造遺失資料。此輪 Windows x64 Debug／Release 的 bag.storage／bag.module／bag.shell 均 **3/3 通過**。
相關程式與 Module 已重新建置；本次未重跑全專案 70 個測試，Linux／macOS 未驗證。
測試涵蓋超過舊四分之一容量、非整頁上限、超出支援範圍時不建目錄、整批回滾、
丟棄等待中的資料，以及容量不足後 Recorder／中央／其他 Module 仍可操作。

# 共用 Socket 控制通訊

`vinc::control` 是中央與子程序共用的通訊元件，連結 `vinc::osal`。
不依賴 `BaseModule`、Shell、AxonBus 或程序重啟政策，也不使用 Pipe。
`Service` 可接收已連接的 OSAL 串流 Socket；目前 Module 使用本機串流端點。
將來框架對框架的連線、驗證與路由尚未實作。

## 責任與執行緒

- 每個 `Service` 使用一個 I/O 執行緒與一個 OSAL `SocketPoller`，共用於其所有會話。
- `Session` 提供具所有權的訊息收發、握手、heartbeat、錯誤與關閉狀態。
- 使用者 callback 不在 I/O 執行緒執行。Session 的公開操作可並行呼叫；物件本身的析構需要外部同步。
- Socket 所有權移交給 Service，只有 I/O 執行緒收發、註冊、解除註冊及關閉。
- Service 析構會關閉會話並 join I/O 執行緒；不等待應用訊息完成交付。
- 程序終止與重啟由上層 ModuleManager／模組執行環境處理。

### 程式碼閱讀順序

- `protocol.hpp`／`protocol.cpp`：v1 封包欄位、編碼、串流分段解碼及參數字串。
- `channel.hpp`：Service／Session 的公開操作、等待與生命週期契約。
- `channel.cpp`：`create_engine()` 準備 poller 與狀態，Service 取得所有權後才由
  `start_worker()` 啟動執行緒；`prepare_peer()` 準備會話，通過容量檢查後才交付 I/O 執行緒。
- I/O 執行緒透過 `advance()` 依序接收（`receive_frames()`）、檢查期限
  （`check_session_deadlines()`）、傳送（`send_frames()`），最後更新 poller 關注事件。
  這些階段共用同一次 peer lock，不在 helper 邊界釋放鎖。
- Engine mutex 保護會話清單與停止狀態；Peer mutex 保護單一會話狀態與佇列。
  需要同時持有兩者時，先取得 Engine mutex，再取得 Peer mutex。

目前仍保留既有 `control::Result`／`control::Error` 與編碼超限時的
`std::length_error` 行為；統一至 common 錯誤介面的遷移須另行處理。

## v1 封包

所有整數採 big endian，不直接傳輸 C++ struct、pointer 或 ABI 物件。

| Offset | 長度 | 欄位 |
| --- | --- | --- |
| 0 | 4 | Magic `VINC` |
| 4 | 2 | 協定版本，v1 固定為 1 |
| 6 | 2 | Kind：Hello=1、Heartbeat=2、Request=3、Response=4、Event=5 |
| 8 | 2 | Domain：Session=0、Lifecycle=1、Command=2、Bus=3 |
| 10 | 2 | Operation，依 Domain 定義 |
| 12 | 8 | Request ID；請求與回覆非零，事件為零 |
| 20 | 4 | Payload bytes，上限 16 KiB |

長度前綴字串使用 u32 byte length + UTF-8 bytes。參數為 u32 count + 字串序列，上限 256 個。
通訊核心把應用 payload 視為 bytes，具體功能負責其內容與字串語意驗證。
Decoder 處理部分標頭、部分 payload 及多則連續訊息，在配置 payload 前檢查大小。

Hello payload 依序為 role(u32)、generation(u64)、token(string)、heartbeat interval(u32 ms)、
heartbeat timeout(u32 ms)。雙方必須角色相反，其他欄位完全相符；不相容的版本或握手會關閉連線。
目前 Module 支援 Lifecycle、Command 與 [Bus 註冊／拓樸協定](../bus/DESIGN.md)。
Bus payload 留在 shared memory；共用控制層仍不解讀上層 Bus 內容。
後續協定演進需明確增加版本或協商欄位，不能直接修改既有 v1 欄位語意。

Heartbeat payload 是從 1 開始、嚴格遞增的 u64 序號。只有有效 heartbeat 更新失聯期限，
一般業務訊息不代替 heartbeat。使用本機 steady clock，不依賴兩端時鐘同步。
heartbeat 只表示通訊執行環境有回應，不表示所有應用功能正常。

## 容量、排程與交付

- 每個方向的應用佇列上限 64 則／256 KiB（含標頭）；傳送容量包含正在部分寫入的封包。
- 傳送滿載立即回報 Backpressure；接收佇列滿載關閉會話並保留 Backpressure 原因。
- heartbeat 使用獨立保留空間，在下一個封包邊界優先發送，不無限堆積過期 heartbeat。
- 每次迴圈限制各 peer 的收發工作量。已部分寫入的封包不能插入 heartbeat，
  OS socket buffer 裡的資料也不能被超越；此方案不提供壅塞下的硬即時 heartbeat 保證。
- `send()` 成功只表示接受進本機佇列，不代表對端收到或執行。
- Request ID 的配置與配對屬於使用此元件的功能協定；Session 不自動重送或重新連線。
- 會話關閉後不能重用。不同程序啟動使用新的 generation 與 token。
- 未知 application Domain 可由共用層傳送；接收功能決定回報 unsupported 或拒絕。

## 安全與限制

Module 的啟動 token 用來綁定本次程序啟動，避免誤接舊會話；不是針對同帳號惡意程序的安全沙箱。
目前 token 由中央透過程序參數傳入。Session 不提供 TLS、遠端認證或機密儲存，不應將此通道直接公開到網路。
本機 Socket 目錄由 ModuleManager 建立與回收；POSIX 目錄設為 owner-only。

`control.channel` 測試封包分割／合併、版本及長度拒絕、雙向 heartbeat、錯誤握手、
Socket 斷線、佇列背壓與連線仍存活時的 heartbeat 逾時。
另以固定 v1 bytes 驗證封包相容性，涵蓋字串內嵌 NUL／UTF-8、截斷與多餘資料拒絕，
以及等待逾時、會話容量拒絕和 Session 比 Service 長壽時的關閉行為。

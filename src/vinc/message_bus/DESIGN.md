# Message Bus 實作紀錄

本文記錄目前實作及驗證邊界，公開使用契約見 [README](README.md)。

## 責任

- `BusManager` 在中央專用執行緒管理 topic schema、Publisher／Subscriber 註冊、共享記憶體名稱、
  reader index、預算及回收。ModuleManager 的監督執行緒只送控制工作、通知已確認程序退出。
- Module watchdog 是 Session application frame 的唯一讀取者，依 Domain 路由；Message Bus Client 以
  request ID 配對回覆，Attach／Detach 只更新小型 topology，mapping 在 Subscriber 呼叫執行緒完成。
- `Publisher`、`Subscriber` 在子程序直接操作 OSAL mapping。資料發布／讀取不經中央 BusManager 或 Socket。
- `message_storage` 負責生成 class 的 offset 映像、容量與驗證；`adopt_record` 接管已複製的 buffer，
  避免接收端再次複製整筆映像。C++ class、虛擬函式表與程序內的所有權物件不放在共享區。

## RingBuffer 布局與原子操作

每個 Publisher 有 control 與 payload 兩個命名共享區；topic 另有共用 counter。
固定 header 128 bytes，每 slot metadata 64 bytes，每訂閱 reader token 64 bytes。
payload 有 N 個固定 byte budget 的位置，stride 對齊 8 bytes。控制區版本為 1，
magic、schema、dimensions、instance ID 在 open 時核對；訊息映像維持 message_storage v2。

不可變 header 在對外註冊前建好；運作中的共享欄位一律透過 OSAL `SharedWord` 存取。
OSAL 提供 native sequentially-consistent 64-bit load/store/CAS/add，不使用程序內 mutex fallback，
也不使用可能依賴程序內位址的 `std::atomic::wait/notify`。Windows 的 atomic load 以 Interlocked RMW 實現，
所以控制區各端都需 ReadWrite；Subscriber 的 payload mapping 是 ReadOnly。

gate 高 62 bits 是該 RingBuffer 全域遞增的 reservation generation，低 2 bits：

| 值 | 狀態 |
| --- | --- |
| 0 | Empty |
| 1 | Ready |
| 2 | Draining：拒絕新讀者，等待既有讀者 |
| 3 | Writing：只有 Publisher 可寫 |

generation 每次成功保留位置就增加，包含之後發布失敗的 reservation；不會因重用不同 slot 而重複。
每 slot 有自身 sequence、timestamp、topic count、image size。RingBuffer head 是最後成功發布的 sequence。
`gate=Ready` 後最後更新 head，這次 head store 是 publication point；讀者不接受 sequence 大於所觀察 head 的 slot。
正常Publisher操作在本機序列化，所以 head 按自身FIFO增加。

## 保護複製的協定

1. Reader 原子讀取候選 gate、metadata，再驗 gate 未變。
2. Reader 在其專屬 reader index 寫入該候選的完整 gate token，再驗 gate 仍相同且 RingBuffer alive。
3. 成功則只執行 payload memcpy；完成清零 token。失敗清零並重新選候選。
4. Writer 把 Ready CAS 成同 generation 的 Draining，逐一檢查 reader tokens。
5. 仍有該舊 token 時等待；全數釋放後配置新 generation、Writing，才開始修改 payload。

SC 順序使「Reader 宣告／再驗」與「Writer 關閉／掃描」可比較：
若 Reader 再驗先於關閉，Writer 掃描會看到其 token，除非 Reader 已完成並清零；
若 Writer 掃描先於 Reader 宣告，Reader 再驗必須看到 Draining 或新 generation，不能開始複製。
因此 Writer 不會覆寫正在成功保護的 payload。metadata 本身也是原子 word，候選檢查不先碰非原子 payload。
新讀者無法延長已關閉 slot 的舊世代，避免不斷加入讀者造成該回收等待永遠清不完。

這不保證 OS 一定排程 Reader，也不保證 memcpy 有固定時間上限。等待中 yield 後每 1 ms 再檢查，
不是忙等霸占核心，也不是 wait-free。碰撞統計僅計 Writer 確實觀察到占用的等待。

reader index 屬於訂閱而非執行緒或 OS PID；同一 Subscriber 的 read 序列化。
中央只在該訂閱已正常 quiescent 退訂，或其唯一 process generation 確認退出後清 token／重用 index。
Heartbeat timeout 自己不足以安全清 token；仍活著但掛住的讀者可能讓 Writer 持續等待，
需由 Module 故障政策終止並確認退出才能回收。中央控制面、heartbeat 與 reader copy 不執行應用 callback。

## Message Bus Domain=3 的 v2 控制協定

沿用共用 Socket v1 framing、big-endian 整數與 u32 長度字串。
Operation 的高 byte 為 Bus 版本 2，低 byte 為操作編號；v1 的 1–6 已退休。
中央 `submit()` 在工作入列前驗證，Client dispatcher 在處理回覆／拓樸前驗證，
舊／未知版本拒絕並由接線關閉會話，不做 capability negotiation 或舊格式 fallback。
新 Client 的 v2 request 對舊 BusManager 是未知操作，舊錯誤回覆亦無法通過新 envelope 驗證。
此變更不修改共用 Socket framing、當時的 Module bootstrap（目前版本見 Module System）、Lifecycle／Command 欄位。

| Operation | Kind | Payload |
| --- | --- | --- |
| `0x0201` Advertise | Request | topic string、schema u64、requested slots u32、byte budget u64 |
| `0x0202` Subscribe | Request | topic string、schema u64、start u32（0 oldest、1 new-only） |
| `0x0203` ClosePublisher | Event | Publisher ID u64 |
| `0x0204` Unsubscribe | Event | Subscription ID u64 |
| `0x0205` Attach | Event | Subscription ID u64、Attachment |
| `0x0206` Detach | Event | Subscription ID u64、Publisher ID u64 |

Response 沿用 request／operation，先放 u32 tag：0 成功、1 失敗，其他值拒絕。
失敗 body 為 Error envelope：u32 version=1、u32 record count（1..16）、u32 flags
（0 或 1；1 表示省略中間原因）。每筆 record 依序為 domain u32、code i32、operation i32、
native source u32、native value i64，共 24 bytes。全部 big endian，有號數保留位元表示。
第一筆 domain 必須為 MessageBus，原因 domain／code／operation／native source 的未知值保留；
目前層的未知錯誤碼與操作碼亦保留，不以 enum 最大值拒絕。

錯誤版本、非法 tag／flags／筆數、截斷、額外 bytes，以及只有一筆卻宣告省略原因均拒絕。
wire 最多 16 筆，不隨本機原因鏈容量改變；超出 wire／本機容量時保留目前層、最近原因與最深原因，
並標記省略。完整驗證後才重建 Error，不直接傳輸 C++ 布局。
合法的遠端 `Protocol` 錯誤與損壞封包是不同結果：前者交付呼叫端，後者關閉會話。
成功 body 不變：
Advertise 成功附 Descriptor；Subscribe 成功附 subscription ID u64、count u32、Attachments。
Descriptor 依序為 id、owner generation、schema、rank（皆u64），slots、readers（u32），bytes（u64），
control name、payload name、counter name、publisher name（strings）。Attachment 再接 reader index u32、floor u64。
floor 是該來源初始游標，只影響 callback 批次讀取。

接收 Subscribe Response 時先建立 topology 才喚醒等待者，避免後續 Attach 先被當成未知訂閱而丟失。
正常 Unsubscribe 後抵達的舊拓樸事件可忽略。Publisher retire 與讀者尚未處理 Attach 可競爭；
mapping 已退役或已收到 Detach 時跳過，其他 OS 錯誤回傳；稍後重試會觀察更新後 topology。
不以映射／註冊失敗假裝「沒有資料」。控制背壓或 RPC 逾時採關閉會話，避免未知結果被重試成重複註冊。

## 驗證對照

- `message_bus.cleanup`：請求送出／回覆複製／註冊解碼／部分映射後的配置失敗、pending 名額回收、
  已知 ID 回滾與未知結果關閉、畸形 Advertise 成功回覆拒絕、單一註冊名額及精確預算重用。
  跨 topic 退出案例驗證 Detach 例外／Result 失敗不阻斷其他通知或死亡 Reader token 清理。
  使用同元件原始碼的編譯期測試入口、真實 Socket 與 mapping；退出 owner 為停止存取的 fixture，
  不代表實際 OS 記憶體耗盡。詳細邊界見 [README 重審修正](README.md#2026-10-05-重審修正註冊回滾與退出回收)。

- `message_bus.error_protocol`：原因鏈 round-trip、未知負數代碼、不同接收容量截斷、
  靜態 catalog、Storage 轉接、非法版本／tag／flags／筆數／長度／目前層 domain，
  以及中央在入列前拒絕 v1 與未知操作。
- `message_bus.error_process`：父程序透過真實 Socket 回覆合成 OSAL 原因、未知 Bus 代碼、
  截斷鏈與合法 Protocol 錯誤；子程序經 Client 訂閱／Runtime dispatcher 取得並檢查完整 Error。
  驗證失敗回覆不建立拓樸，v1 Detach 事件被拒絕，以及請求編碼失敗保留 Control Channel 原因。
  此 fixture 注入數字診斷，沒有注入 OS 資源耗盡。

- `message_bus.ring_buffer`：不同程序／不同虛擬位址的共用 counter（4 × 20,000 次）、同主機時間，
  timestamp／Publisher順位比較、schema與溢位拒絕、gate關閉拒絕新讀者、保護中不覆寫、
  ABA世代拒絕、保留最新、慢讀者跳過、碰撞統計、3,000次併發payload完整性、
  非8倍數byte上限、世代及counter耗盡拒絕。
  可讀性整理另補控制區與 payload 名稱衝突，確認建立失敗只回收本次建立的資源。
- `message_bus.module`：真正獨立Module程序，多Publisher加入與退出、2 MiB及巢狀生成訊息，
  本機push／直接loan、schema不符／超量拒絕、未commit／丟棄loan仍禁止再借，
  oldest／new-only游標、last 不推進內部批次游標、讀者落後、Publisher崩潰隔離及重新加入。
  測試專用入口將讀者停在保護複製邊界，讓Writer等待超過heartbeat timeout；
  驗證heartbeat持續，再讓讀者程序崩潰，確認中央回收lease後Writer繼續並記錄等待時間。
- `message_bus.shell`：真實Shell啟動兩個messageBusExample程序，執行push、重複last、loan、commit、callback 與 stats。

測試有 CTest 總時間限制及 OSAL 子程序回收。通過測試不代表窮盡所有排程／故障時序，
亦不構成Linux／macOS驗證、吞吐量結果或硬即時保證。

### 本次執行結果

2026-10-05 回收修正：Windows Debug／Release 相關目標重建成功，六項 Message Bus 測試與
`module.process` 各 7／7 通過（24.61 秒／20.27 秒）。Linux／macOS 未驗證。

2026-10-05 統一錯誤遷移：Windows Debug／Release 完整建置及相關測試各 17／17 通過；
最終 Debug 的兩個錯誤測試另重跑通過。範圍、工具鏈與限制見 [README 本輪驗證](README.md#本輪驗證2026-10-05)。

2026-10-03 可讀性整理：Windows x64／MSVC Debug、Release 完整建置及 SDK header smoke 通過，
CTest 各 26／26 通過（46.91 秒／40.78 秒）。RingBuffer 名稱衝突回收案例納入既有 `message_bus.ring_buffer`。
共享布局欄位、控制 codec 與關鍵原子操作順序已比對；Linux／macOS 未執行。

2026-10-01：Windows x64、MSVC 19.51.36257、Windows SDK 10.0.26100.0、
CMake 4.3.2、Ninja 1.13.2。Debug／Release 都完整重建，最終兩種組態各 15 套 CTest 全部通過。
Debug 總測試時間 24.52 秒，Release 20.81 秒；這是測試執行時間，不是效能基準。
沒有執行 Linux／macOS 原生驗證，沒有建立跨平台部署保證。

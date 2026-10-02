# Bus v1 實作紀錄

本文記錄目前實作及驗證邊界，公開使用契約見 [README](README.md)。

## 責任

- `Broker` 在中央專用執行緒管理 topic schema、Publisher／Subscriber 註冊、共享記憶體名稱、
  reader index、預算及回收。ModuleManager 的監督執行緒只送控制工作、通知已確認程序退出。
- Module watchdog 是 Session application frame 的唯一讀取者，依 Domain 路由；Bus Client 以
  request ID 配對回覆，Attach／Detach 只更新小型 topology，mapping 在 Subscriber 呼叫執行緒完成。
- `Publisher`、`Subscriber` 在子程序直接操作 OSAL mapping。資料發布／讀取不經中央 broker 或 Socket。
- `message_storage` 負責生成 class 的 offset 映像、容量與驗證；`adopt_record` 接管已複製的 buffer，
  避免接收端再次複製整筆映像。C++ class、虛擬函式表、owner 與 wrapper cache 不放在共享區。

## Ring 布局與原子操作

每個 Publisher 有 control 與 payload 兩個命名共享區；topic 另有共用 counter。
固定 header 128 bytes，每 slot metadata 64 bytes，每訂閱 reader token 64 bytes。
payload 有 N 個固定 byte budget 的位置，stride 對齊 8 bytes。控制區版本為 1，
magic、schema、dimensions、instance ID 在 open 時核對；訊息映像維持 message_storage v2。

不可變 header 在對外註冊前建好；運作中的共享欄位一律透過 OSAL `SharedWord` 存取。
OSAL 提供 native sequentially-consistent 64-bit load/store/CAS/add，不使用程序內 mutex fallback，
也不使用可能依賴程序內位址的 `std::atomic::wait/notify`。Windows 的 atomic load 以 Interlocked RMW 實現，
所以控制區各端都需 ReadWrite；Subscriber 的 payload mapping 是 ReadOnly。

gate 高 62 bits 是該 Ring 全域遞增的 reservation generation，低 2 bits：

| 值 | 狀態 |
| --- | --- |
| 0 | Empty |
| 1 | Ready |
| 2 | Draining：拒絕新讀者，等待既有讀者 |
| 3 | Writing：只有 Publisher 可寫 |

generation 每次成功保留位置就增加，包含之後發布失敗的 reservation；不會因重用不同 slot 而重複。
每 slot 有自身 sequence、timestamp、topic count、image size。Ring head 是最後成功發布的 sequence。
`gate=Ready` 後最後更新 head，這次 head store 是 publication point；讀者不接受 sequence 大於所觀察 head 的 slot。
正常Publisher操作在本機序列化，所以 head 按自身FIFO增加。

## 保護複製的協定

1. Reader 原子讀取候選 gate、metadata，再驗 gate 未變。
2. Reader 在其專屬 reader index 寫入該候選的完整 gate token，再驗 gate 仍相同且 Ring alive。
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

## Bus Domain=3 的 v1 控制協定

沿用共用 Socket v1 framing、big-endian 整數與 u32 長度字串。
目前兩端需要同版 framework；此版本新增原本保留的 Domain，不改 Lifecycle／Command 欄位。
尚無 Bus capability negotiation；舊 runtime 遇不支援的 Bus 操作會拒絕／關閉，不降級傳 payload。

| Operation | Kind | Payload |
| --- | --- | --- |
| 1 Advertise | Request | topic string、schema u64、requested slots u32、byte budget u64 |
| 2 Subscribe | Request | topic string、schema u64、start u32（0 oldest、1 new-only） |
| 3 ClosePublisher | Event | Publisher ID u64 |
| 4 Unsubscribe | Event | Subscription ID u64 |
| 5 Attach | Event | Subscription ID u64、Attachment |
| 6 Detach | Event | Subscription ID u64、Publisher ID u64 |

Response 沿用 request／operation，先放 status u32：零為成功，非零對應 `ErrorCode` 再附錯誤字串。
Advertise 成功附 Descriptor；Subscribe 成功附 subscription ID u64、count u32、Attachments。
Descriptor 依序為 id、owner generation、schema、rank（皆u64），slots、readers（u32），bytes（u64），
control name、payload name、counter name、publisher name（strings）。Attachment 再接 reader index u32、floor u64。
floor 是該來源初始游標，只影響 next。

接收 Subscribe Response 時先建立 topology 才喚醒等待者，避免後續 Attach 先被當成未知訂閱而丟失。
正常 Unsubscribe 後抵達的舊拓樸事件可忽略。Publisher retire 與讀者尚未處理 Attach 可競爭；
mapping 已退役或已收到 Detach 時跳過，其他 OS 錯誤回傳；稍後重試會觀察更新後 topology。
不以映射／註冊失敗假裝「沒有資料」。控制背壓或 RPC 逾時採關閉會話，避免未知結果被重試成重複註冊。

## 驗證對照

- `bus.ring`：不同程序／不同虛擬位址的共用 counter（4 × 20,000 次）、同主機時間，
  timestamp／Publisher順位比較、schema與溢位拒絕、gate關閉拒絕新讀者、保護中不覆寫、
  ABA世代拒絕、保留最新、慢讀者跳過、碰撞統計、3,000次併發payload完整性、
  非8倍數byte上限、世代及counter耗盡拒絕。
- `bus.module`：真正獨立Module程序，多Publisher加入與退出、2 MiB及巢狀生成訊息，
  本機push／直接loan、schema不符／超量拒絕、未commit／丟棄loan仍禁止再借，
  oldest／new-only游標、last不推進next、讀者落後、Publisher崩潰隔離及重新加入。
  測試專用入口將讀者停在保護複製邊界，讓Writer等待超過heartbeat timeout；
  驗證heartbeat持續，再讓讀者程序崩潰，確認中央回收lease後Writer繼續並記錄等待時間。
- `bus.shell`：真實Shell啟動兩個busExample程序，執行push、重複last、loan、commit、next與stats。

測試有 CTest 總時間限制及 OSAL 子程序回收。通過測試不代表窮盡所有排程／故障時序，
亦不構成Linux／macOS驗證、吞吐量結果或硬即時保證。

### 本次執行結果

2026-10-01：Windows x64、MSVC 19.51.36257、Windows SDK 10.0.26100.0、
CMake 4.3.2、Ninja 1.13.2。Debug／Release 都完整重建，最終兩種組態各 15 套 CTest 全部通過。
Debug 總測試時間 24.52 秒，Release 20.81 秒；這是測試執行時間，不是效能基準。
既有 legacy `test_module.cpp` 的未使用參數 warning 仍在。
沒有執行 Linux／macOS 原生驗證，沒有建立跨平台部署保證。

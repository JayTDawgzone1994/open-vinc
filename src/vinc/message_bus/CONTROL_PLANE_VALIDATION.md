# Bus control-plane validation / Bus 控制面驗收

These are dated validation records, not current implementation steps.
歷史結果與原始失敗保留當時範圍；文中的「待驗」不代表目前仍未完成。
Current contract / 現行契約：[contract](CONTROL_PLANE_REPAIR.md)。
Latest native macOS coverage / 後續 Mac 範圍見 [2026-11-19 report](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)。

## 第一階段回歸結果（2026-11-13）

Windows x64 Release（MSVC 19.51.36257）與 WSL2 Ubuntu 24.04 x86-64 Release（GCC 13.3）各跑三輪。測試以修復後應成立的行為為斷言，保留一般 CTest 失敗狀態，未設 `WILL_FAIL`。每輪 Windows 的 10 個選定案例有 5 個命中缺陷，WSL 的 13 個有 7 個命中缺陷，其餘對照全部通過。Debug、macOS 與完整專案測試尚未驗收。

| 案例 | 施加條件與實測結果 |
| --- | --- |
| `topology-attach`、`topology-detach` | 暫停同一個訂閱 Host 的控制訊息派送，Socket 與心跳繼續執行。讓另一條 Session 逐筆完成 80 個 Topic 的 Publisher 登記或回收，維持慢讀窗口後再恢復派送。兩平台每輪都交付了前 64 幀，接著在 Receive 回報 Control Channel 容量耗盡。相同流程只變更一個 Topic 時，來源集合正確且 Session 保持開啟。此案例驗證接收佇列；原調查的單一 Topic 多訂閱負載仍列在整合驗收。 |
| `timely-request`、`expired-request` | 真實回收 ACK 喚醒後，測試版同步點可讓該呼叫執行緒延後到自己的單調時鐘期限再繼續。正常 ACK 對照能完成新訂閱。延後組每次都觀察到傳輸層接受 1 筆過期 Subscribe，隨後回 Request Timeout、Session 關閉。斷言要求接受 0 筆，並在原 Session 重試成功。 |
| `reservation-ring-control`、`reservation-ring-payload` | 在既有 Topic 上分別令 RingBuffer 的 control 與 payload 名稱碰撞。測試 journal 核對實際保留的名稱集合，兩平台的原生共享物件皆可重新開啟且內容完整。control 失敗留下 2 筆額外登記，payload 失敗留下 1 筆。無碰撞的登記與退場回到原集合。 |
| `emfile-control`、`emfile-payload`（POSIX） | 在 WSL 的真實中央程序保留零或一個空 FD，各拒絕三次，最底層錯誤均為 OSAL CreateSharedMemory / EMFILE。journal 從 9 增至 15 或 12，正常關閉後各剩 6 或 3 筆。每次拒絕後既有 Topic 都能收發新序號，恢復 FD 後能再次建立 Publisher。既有 Subscriber-only 計數器測試通過。跨程序錯誤鏈可能省略中間操作；上列同名碰撞案例另在 Bus 邊界直接驗證 CreateControl 與 CreatePayload。 |

在開啟測試的建置目錄執行 `ctest -R "^message_bus\.(control_plane\.|cleanup$|counter_registration$|topic_registration_emfile$)" --output-on-failure`。測試註冊與平台條件見 [CTest 清單](tests/CMakeLists.txt)。

## 第二階段驗收（2026-11-13）

正常 ACK 對照成功。呼叫端在 ACK 已確認後延至期限才恢復，Windows 與 WSL 均記錄 `expired Subscribe admissions=0 session_open=1`。同一 Session 隨後使用回收批次的下一個 request ID 訂閱成功。另驗證了等待 ACK 本身逾時、已送達中央的變更遺失回覆，以及唯讀查詢逾時後收到晚到回覆的行為。期限與錯誤分類見 [端點回收](DESIGN.md#endpoint-release)。

- Windows x64（MSVC 19.51）Release：`control_plane.timely-request`、`control_plane.expired-request`、`cleanup`、`error_process` 四項通過。既有程序持有 Release 測試 Host，致整合產物無法重新連結（LNK1104）；`release_routes` 使用重新建置的 Debug 產物通過。
- WSL2 Ubuntu 24.04 x86-64（GCC 13.3）Release：上述五項全部通過。macOS 原生與完整專案測試未執行。

## 第三階段驗收（2026-11-13）

- Windows x64（MSVC 19.51）Release：RingBuffer、cleanup、counter registration 與四個 reservation 案例共 7/7 通過。control 與 payload 分別撞上既有 mapping 時，各自回報原本的建立錯誤，journal 名稱集合恢復基線，另一個擁有者仍可開啟並更新原物件。第二筆預留返回失敗或拋出配置例外時，第一筆只撤銷一次。模擬 journal 同時拒絕兩筆退役時，中央保留兩個名稱的清理診斷，呼叫端仍收到原始碰撞。另以 Debug 通過雙 App 崩潰重啟隔離測試。
- WSL2 Ubuntu 24.04 x86-64（GCC 13.3）Release：上述共通案例、重啟隔離及三項實際 FD 耗盡測試共 11/11 通過。中央剩零或一個可用 FD 時，各連續拒絕三次新增 Publisher，每次 journal 均為 9 → 9，既有 Topic 可傳遞下一筆資料。恢復 FD 後重新註冊成功，關閉後兩組均剩 0 筆。
- Linux 測試執行檔對 control、payload 各自注入 ftruncate 或 mmap 失敗，分別允許或拒絕該名稱的 unlink。允許時兩筆預留全數撤銷；拒絕時只保留該名稱，實測仍可開啟。原生主因保留 ENOSPC 或 ENOMEM，清理診斷為 EACCES。首輪注入未匹配 OSAL 的原生名稱前綴；修正測試並重新建置後通過。
- macOS 原生與完整專案測試本輪未執行。拓樸佇列與補查屬第四階段。

## 第四階段：延後套用更新與第一次開啟共享區

Windows x64 MSVC 19.51 Release 與 WSL Ubuntu 24.04 GCC 13.3 Release 均可透過
原診斷探針重現，現已改為 `message_bus.control_plane.retired-source` 回歸測試。在真實 Session 上完成訂閱，等中央回收 Publisher，
暫留已收到的 Detach，再呼叫 `get_last()`。Windows 回 Win32 2，Linux 回 ENOENT；
Bus 的最外層為 OpenControl，底層為 OSAL OpenSharedMemory / NotFound。套用 Detach 後同一個
Subscriber 回 NoPublishedData。此案例未使佇列滿載，也未注入 OS 故障。

修正前，callback 訂閱的 `capture()` 亦回 OpenControl。現已實作[唯讀來源確認](DESIGN.md)：
中央回覆前，`capture()` 與 `get_last()` 均可完成；同 Topic 的另一個 Publisher 可在該輪交付。
`confirmed-source` 另以受控回覆確認兩條路徑：映射恢復時可重新接上，仍然缺少時保留原生錯誤。

2026-11-13 定向驗收：先在 WSL 確認新斷言可抓到舊版 OpenControl／ENOENT。
Windows x64 MSVC 19.51 Release 的八項 Bus 回歸通過，上述兩個新案例另各連跑五次通過；
Release 整合測試 Host 因 LNK1104 無法重新連結，改用 Debug 重建 Plugin entry 21 的 Host／
Plugin，跨程序 `message_bus.module` 通過。WSL2 Ubuntu 24.04 GCC 13.3 Release 九項通過，
包含相同八項與跨程序整合。macOS 原生與完整專案測試尚未執行。

分頁與 ACK 的編解碼、收送頁序以及多筆訂閱共用一份 Publisher 描述已有獨立測試。
Windows 與 WSL Release 的 `message_bus.error_protocol`、`message_bus.cleanup`、
`message_bus.notification_snapshots` 各 3/3 通過。上述為接線前的 v5 基線，未涵蓋滿載後的端到端收斂。

## 第四階段正式接線與驗收（2026-11-14）

Bus v6／Plugin entry 22 已將版本頁面接上中央工作佇列與 Host dispatcher；請將中央、Host、
Plugin 與 SDK 一起重建。傳輸、暫存與登記切換的細節見 [Bus 協定](DESIGN.md)。

| 案例 | 實際驗證的行為 |
| --- | --- |
| topology-attach／detach | 在一個 Host 暫停派送時，另一個 Host 逐筆完成 80 個 Topic 的變更。恢復後無新變更，原 Session 繼續開啟，逐筆來源正確且 applied 等於 target。 |
| topology-snapshot | 160 個 Topic 需要多頁。接收非末頁時舊視圖仍完整；暫扣頁面 ACK，關閉一個舊訂閱、跨過快照邊界建立新訂閱，再移除尚未送達的 Publisher。原版本包含當時存在的來源，排除額外 Topic；之後的版本反映移除。 |
| topology-replace | 同一受控流程，在保存未讀 Topic 的舊資料時注入配置失敗。中央讓已送頁安全結束，再用新傳輸序號補查；Host 捨棄半份舊快照，原 Session 收斂。 |
| topology-admission | 在中央第一次送頁與 Host 第一次回 ACK 分別注入 Backpressure。中央自行再送，Host 透過既有 watchdog 入口再送 ACK。兩次拒收均未重播 Registry 操作。 |
| module／release_routes／shutdown_evidence | 真實程序回歸，涵蓋 Bus 通訊、端點回收與 Host 退出派送順序。 |

分頁數、bytes、常駐記憶體、持續變動時的公平性及收斂耗時屬第五階段；
上述定向測試不能替代成本對照。macOS 原生與乾淨 installed SDK 待驗。

最終定向測試使用 Windows x64 MSVC 19.51.36257（Release 17/17、Debug 跨程序 3/3），
以及 WSL2 Ubuntu 24.04 x86-64 GCC 13.3（Release 22/22）。WSL 首輪兩個 EMFILE 案例
使用前版測試執行檔，中央拒絕協定版本；重建 `message_bus_topic_registration_tests`
及其相依後，同樣條件的 control／payload 兩例均通過。此輪未跑完整專案 CTest。

## 第五階段：整合與成本驗收（2026-11-16）

### 範圍與環境

本輪新增測試及 `VINC_MESSAGE_BUS_TESTING` 下的計數接線，沒有新增正式背景執行緒、
監控或改變第四階段 Runtime 行為。測試基線為 `fc27d2788e74a163ea597f4e736004dbcfc6d529`
加上本契約各階段的工作樹。最終原始碼 SHA-256 清單保存在忽略的
`build/topology-stage5-source-manifest.json`，提交後應由驗收者另記實際 commit。

| 環境 | 相關 CTest | 乾淨 C++ installed SDK |
| --- | --- | --- |
| Windows 11 x64，build 26200，MSVC/STL 19.51.36257，CMake 4.3.2，Debug | 30/30，206.45 s | consumer、application 與搬移通過 |
| 同上，Release | 30/30，199.65 s | consumer、application 與搬移通過 |
| WSL2 Ubuntu 24.04.5 x86-64，GCC/libstdc++ 13.3.0，glibc 2.39，CMake 3.28.3，Debug | 33/33，140.10 s | consumer、application 與搬移通過 |
| 同上，Release | 33/33，130.12 s | consumer、application 與搬移通過 |

WSL kernel 為 `6.18.58.1-microsoft-standard-WSL2`。Linux 多出的三項是實際 FD 耗盡案例，
不以 Windows 的受控失敗代替。這是相關測試矩陣，**不是全專案 CTest**，也未在 macOS 執行。
兩平台兩組態的七項拓樸案例另各連跑三次通過；最後補上的量測欄位及預算斷言，
由重建後的 Debug 一輪、Release 三輪 `topology-load` 與 snapshot／replace 案例驗證。

### 新增案例真正驗證什麼

- `message_bus.control_plane.topology-load` 使用真實 Socket、Control Channel 心跳、
  BusManager worker 與 Client。測試驅動執行緒模擬 Host dispatcher 的可控暫停，
  **不是額外的正式 Runtime worker**。慢 Host 暫停 2,200 ms，超過測試心跳期限 2,000 ms；
  心跳仍由 Control Channel 推進。版本落後不關閉 Session，恢復後即使沒有後續變更也會收斂。
- 先訂後發的 1 Publisher × 128 readers 與 16 × 128 readers，核對每筆 lease 的唯一性、
  初始 floor、保留資料及最後的空來源集合。快 Host 的獨立 Topic 在逐筆登記間持續收發與查詢，
  只收到自己的關係；慢 Host 不會看到半份更新。
- 暫停觀察者後釋放所有 Publishers，下一筆查詢先等待回收批次 ACK，仍能在觀察者未恢復時完成。
  正常端點回收不等待拓樸套用。160 個不同 Topic 另測歷史超額改走多頁快照，
  以及確認退出後清除所有來源；這裡的 `exited()` 是停止測試端所有存取後的受控交接。
- `message_bus.module` 補上真實獨立程序：16×128 建立／清空／重新訂閱、
  另一個 Host 持有 160 個 Topic 後真正異常退出，存活 Host 仍傳送新資料並可重新註冊。
  原有 `release_routes` 保留同一 callback 內清除 141 筆後立即重訂的驗證。
- `topology-admission` 保留頁面與 ACK 第一次傳送遭拒後、沒有新登記也能重試的受控案例；
  snapshot／replace 保留舊視圖一致性、快照期間增刪及配置失敗後重建。
  RingBuffer、退場來源補查、請求期限、資源預留、退出證據及 Control Channel 回歸均納入上述矩陣。

### 幀數與傳輸成本

下表只計慢 Host 的拓樸頁面與 ACK，含 Control Channel header，不含 TCP/IP、心跳、
註冊請求／回覆或資料 payload。Windows／WSL Release 各三輪的數量一致。
舊版欄位是依**相同關係與舊 wire layout 計算的格式模型**；本輪沒有重新執行 v5 二進位檔。
v5 實際滿載失敗證據保留於第一階段，不可把格式模型宣稱為舊版效能量測。

| 建立後再退場的負載 | v5 逐筆格式模型 | v6 實測含 ACK | 最大單頁（含 header） |
| --- | --- | --- | --- |
| 1 × 128，同一 Topic | 256 幀，27,648 bytes | 2 頁 + 2 ACK，3,980 bytes | 2,772 bytes |
| 16 × 128，同一 Topic | 4,096 幀，442,368 bytes | 6 頁 + 6 ACK，60,664 bytes | 16,292 bytes |
| 160 個不同 Topic，擁有者退出 | 未計算完整舊版對照 | 4 頁 + 4 ACK，26,900 bytes，其中 3 頁屬快照 | 16,320 bytes |

分組共用 Publisher 描述與一頁在途，解除了原先逐 reader 事件灌滿接收佇列的情境。
這不代表任意負載都沒有排程成本；落後 Host 仍需時間接收並建立自己的關係視圖。

### 記憶體、收斂及喚醒

`topology_storage()` 僅在測試版 worker flush 後取樣，測量編碼內容的邏輯 bytes，
不是 allocator capacity、所有暫存副本或實際 heap 峰值。Host 的 `staged_wire_peak`
也是該傳輸已收到的 records bytes，解碼結構另有成本。不同欄位峰值未必同時出現，不能相加稱為總峰值。

| 負載 | 中央歷史取樣峰值 | 中央準備頁取樣峰值 | Host 暫存 records 峰值 |
| --- | --- | --- | --- |
| 1 × 128 | 0 | 2,704 | 2,704 |
| 16 × 128 | 40,560 | 40,560 | 40,560 |
| 160 個不同 Topic | 10,496 | 16,252 | 26,256 |

單獨 `topology-snapshot` 的保存未讀 Topic 舊資料峰值為 238 bytes；
`topology-replace` 在該配置點注入失敗，未保留該副本而重新補查，兩平台均通過。
快照案例歷史／準備頁取樣峰值分別為 10,624／16,284 bytes。
這些案例沒有用滿全 App 4 MiB 歷史預算；它們驗證超過每 Host 歷史限制會轉為快照，
**不是所有 Host 數量下的最大記憶體證明**。現行預算與關係視圖本身的成本仍依 DESIGN。

下表為解除派送暫停到 applied 等於 target 且來源數正確，Release 三輪的中位數及範圍。
測試含私有計數，且與 SDK 建置共用開發機資源，只作本輪成本觀察，不是正式 benchmark、
跨 OS 效能排名或最壞延遲保證。

| 負載 | Windows（ms） | WSL（ms） |
| --- | --- | --- |
| 1 × 128 | 0.382（0.332–0.476） | 0.392（0.304–0.414） |
| 16 × 128 | 3.421（3.063–4.154） | 2.300（2.206–3.278） |
| 160 個不同 Topic | 1.435（1.371–1.689） | 1.097（1.065–1.203） |

每個案例在已送頁、尚未收 ACK 的 2,200 ms 靜止區間，Bus worker 的額外醒來次數均為 **0**。
這只涵蓋等待拓樸 ACK 的 worker，不代表 Control Channel 心跳、測試 driver 或整個程序沒有喚醒，
也沒有量測整個程序的閒置 CPU 百分比。

`tests/topology_cost.py` 另取得**整個測試程序**的記憶體高水位，包含映射、Registry、
測試 driver、CRT/STL 與 allocator。三輪完整 load fixture：Windows peak working set
21,385,216–21,426,176 bytes，peak private commit 7,077,888–7,098,368 bytes；
WSL peak RSS 34,529,280–34,877,440 bytes。平台計數含義不同，不能直接比較，
也不能當成「新版拓樸增加了這麼多記憶體」；沒有同條件舊版程序的記憶體基線。

### 重跑與交付

啟用測試後，重建相關 target，再執行：

```sh
ctest --test-dir BUILD -C Release --output-on-failure -R '^(message_bus\.|module\.(context|shutdown_evidence)$|control\.)'
ctest --test-dir BUILD -C Release --output-on-failure --repeat until-fail:3 -R '^message_bus\.control_plane\.topology-'
python src/vinc/message_bus/tests/topology_cost.py PATH_TO_message_bus_cleanup_tests
python src/vinc/message_bus/tests/topology_cost.py PATH_TO_message_bus_cleanup_tests --scenario topology-snapshot
python src/vinc/message_bus/tests/topology_cost.py PATH_TO_message_bus_cleanup_tests --scenario topology-replace
```

第一行是方便交付的廣泛篩選，可能選到本輪矩陣之外的 App/Shell 案例；應先建置其依賴，
不要把未重建的執行檔當成新版本證據。Debug 重複上述流程。可選成本腳本只使用 Python 標準庫，
不安裝套件、不上傳資料；45 秒到期只終止它自己啟動的 fixture。

乾淨 SDK 使用獨立 producer build，`VINC_INSTALL_SDK=ON`、`VINC_BUILD_APPS=OFF`、
`BUILD_TESTING=OFF`，再執行維護中的 `sdk/tests/installed_consumer.cmake` 與
`installed_application.cmake`。驗證全新 prefix 安裝後搬移、獨立 consumer、
自訂訊息與 Module、App 再搬移、Bus／Bag／UI／Tcl、增量訊息及錯誤組態拒絕。
沒有拿原來源 build 的執行檔冒充 installed SDK 產物。

本機原始紀錄在忽略的 `build/topology-stage5-*-tests.log`、`*-final-*.log`、
`*-cost-*.log`、`*-sdk-*.log` 與各 SDK producer 目錄。
Windows 的 Python SDK 首輪誤用了 PATH 中的 MinGW Python 3.14.4，fixture 假設的
`venv/Scripts/python.exe` 不存在，因此在預期的負面案例之前失敗；已改用官方 CPython，
未放寬 binding 或 Runtime 檢查。

`installed_python.cmake` 最終在 Windows 官方 CPython 3.13.12 與 WSL CPython 3.12.3
各自的 Debug／Release 全部通過，包含 SDK-only Python binding、自訂訊息、
Bus／RPC／Param／pump、C++ 互通、SDK／App 搬移、錯誤 ABI 拒絕、metadata 修復、
訊息來源新增／修改／移除，以及停用 Python 後的產物清理。
這是 installed SDK 的 Python 整合範圍，不代表另跑了全專案所有 Python 測試。

macOS 需以同一提交重建並重跑原生 Debug／Release、負載、SDK 與搬移流程，
另記 compiler、標準庫、SDK、Python 及成本計數。整合後的 macOS 原生結果見
[2026-11-19 補驗](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)。

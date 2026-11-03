# macOS Debug Plugin classification investigation / 分類失敗調查

日期：2026-11-03。狀態：**調查完成，測試修復已實作；本輪驗收見文末。**
以下原始調查與提案保留為歷史證據；後續修復不改正式 Runtime／OSAL 行為契約。

The original classification test applied the same 600 ms startup deadline to ordinary
Plugin rejection and the deliberately stalled loader. Fresh-path native runs
reproduced StartupTimeout instead of the expected Plugin error. A finite-load
experiment confirms this mechanism with both current and original parent-side
OSAL. The original single full-suite failure lacks enough diagnostics for a
unique retrospective attribution. The subsequent test repair and its acceptance
results are recorded separately below; historical results remain unchanged.

## 結論與證據界線

`tests/plugin_tests.cpp::expect_failure()` 把所有拒絕／建構失敗案例的
`ModulePolicy::startup_timeout` 都設為 **600ms**，也用同一個值測故意卡住的 loader。
這使「是否收到正確 Plugin 錯誤」同時依賴「Host 啟動、服務連線、原生載入與回報是否
趕在 600ms 內完成」。八秒的 `wait_running()` 等待期限不會延長這個 Module 啟動期限。

本輪在未改正式程式、未注入延遲的新路徑測試中，取得了四次分類不符：預期
`HookFailed`、`ServiceFailed` 或 `HookException`，實際皆為
`ModuleSystem::StartupTimeout / Ready`，退出皆為 macOS signal 9。
其中一次有其他重跑同時執行；另外三次來自已停止重跑後的循序對照。
這證明短期限會搶先決定分類，並非只能推測的原因。

已確認的問題是**分類測試混入過短的啟動時限要求**。對這些重現，中央保存
StartupTimeout 並終止尚未 Ready 的 Host 符合現行政策，不能為了讓測試變綠而讓
後來的 Plugin 錯誤覆寫同 MID 的第一個主要故障。

原始 Debug 全套的單次失敗只有 `failure category retained`，未記 fixture 或實際分類。
本輪找到了相同斷言的可重現機制，但**無法唯一證明原始那次也必定是同一原因**。
原始 Debug **151/153**、Release **152/153** 紀錄保持不變，見
[退出修復第三步報告](../osal/MACOS_EXIT_VALIDATION.md#plugin-failure-classification--plugin-故障分類)。

## 環境與方法

- macOS 26.3.1(a) / 25D771280a，Apple M1 Pro / arm64；Apple Clang 21
  (`clang-2100.1.1.101`)、libc++ 210106、SDK 26.5。
- 基底 `a19c0d456efa8f4ad95f4570e8b43f1b6daf1e00` 加尚未提交的
  [退出觀察修復](../osal/PROCESS_EXIT_PROGRESS.md)，不是單獨 HEAD 的結果。
  重用第三步 Debug 建置；前後以 `source-sha256.json` 核對非文件輸入未變。
- 所有診斷副本、有限延遲 fixture、提案實驗及原始輸出都在 Git 外。
  未修改正式 library、維護中的 test source 或原始 CTest 結果。
- 每次執行有 90 秒外層上限；保留八秒等待、原分類斷言、拒絕安全退出斷言及 cleanup。
  除明確標示的提案實驗，startup policy 保持原來 600ms。
- 新路徑測試僅逐檔複製八個 Plugin，SHA-256 與原檔相同。未 purge 系統快取、
  未停用系統保護、未加 CPU 壓力；「新路徑首次使用」不等同已控制的 OS 冷快取。
- 時間點只用於判定期限與事件順序；沒有 benchmark 或效能保證。

### 實際結果

| 實驗 | 結果 | 解讀 |
| --- | --- | --- |
| 原路徑、只在失敗時印診斷的 Debug 副本 | 215/215 通過 | 暖路徑重跑無法揭露此機制；不能抵銷原始失敗 |
| 新路徑、每案例結束印時間的副本 | 1 次失敗 | null factory 預期 HookFailed，實際 StartupTimeout；此時暖路徑 driver 尚在執行 |
| 新路徑的五組「首次＋同路徑再跑」，僅失敗時印診斷 | 7 通過、3 失敗 | 三次均為 StartupTimeout；本組循序執行，無上述 driver 並行 |
| 外部 Host／Manager 時間線：原路徑 1 次、新路徑 10 次 | 11/11 通過 | 非受控重現；提供載入／回報時間線，沒有在這組捕獲自然失敗 |
| null factory 增加 700ms 有限載入延遲、原 600ms 測試政策 | 現行父端 OSAL 3/3、舊父端 OSAL 3/3 均重現失敗 | 預期 HookFailed，實際 StartupTimeout／signal 9 |
| 同一有限延遲 fixture、外部分類預算 5 秒提案 | 3/3 通過 | 收到 HookFailed 與 InvalidFactory cause；未延長 stall 案例 |
| 受控時間線：原政策／提案各一次 | 原政策失敗、提案通過 | 下節提供先後次序 |
| 提案副本、未加延遲的新路徑五組 | 5/5 通過 | 全部原分類、safe-exit、cause、stall、有效 Plugin 與析構斷言保留 |

暖路徑原規劃最多 500 次、遇失敗停止；取得新路徑失敗後，主動停止 driver 改做循序對照。
已完成的 215 次計入；正在執行的第 216 次讓其自然收尾，但不計入 driver 已完成結果。
沒有為停止 driver 而殺死測試子程序。`repeat-stop.json` 記錄此截斷。
首次新路徑失敗後，再跑同目錄仍可能首次碰到前一輪尚未走到的 fixture；不能把該輪
所有 Plugin 都稱為已暖機。

三次循序自然失敗分別是 `fresh-0` 的 `plugin_reject`、同目錄再跑的
`plugin_throw_constructor`，以及 `fresh-3` 的 `plugin_late_coordinator`。
失敗不是固定的 Plugin entry 檢查分支。Log shutdown diagnostic 也出現在通過案例，
不能用它代替 Module 的實際分類。

舊父端 OSAL 對照只替換測試父程序的 OSAL archive（來自原 HEAD），Manager、Host、
Plugin 與其他 library 保持本輪版本。它證明**這個受控機制不需要新的父端退出修復**，
不是整份舊版本驗收，也不代表已排除所有退出修復回歸。

## 根因定位：期限先於 Plugin 拒絕

相關位置：

1. `tests/plugin_tests.cpp::expect_failure()`：所有失敗案例套用 600ms。
2. `src/host/module_manager.cpp::launch()`：在 `spawn_child()` 前設定 startup deadline。
3. `src/runtime/module_runtime.cpp::run_module_runtime()`：控制連線、Log 初始化、
   通知資源與 `source.prepare()` 都在 Plugin 故障回報之前。
4. `src/runtime/plugin_loader.cpp::LoadedPlugin::load()`：先執行原生
   `DynamicLibrary::open()`，返回後才檢查 entry／初始化；null factory 與建構子錯誤更晚。
5. `ModuleManager::tick()`：期限已到且仍 Starting 時保存 StartupTimeout；
   `save_failure()` 保留同 MID 第一個主要故障。逾時後會要求終止，再等待真實退出。

未加延遲的時間線中，原路徑已使用的測試 Plugin `open()` 均小於 1ms；新路徑
有完成的 `open()` 到約 501ms。這些數值只說明為何連同 Host 前置作業容易逼近 600ms，
不是效能比較。原生載入內部包含哪些 OS 工作，本輪沒有系統層 trace，
**不能斷言是防毒、簽章驗證、dyld cache 或某項 macOS 缺陷**。

為直接驗證先後關係，外部 null-factory fixture 在靜態初始化增加有界 700ms sleep。
這是明確標記的注入，不冒充自然失敗。正式程式及錯誤回報協定沒有改動。
`controlled-trace-original.txt` 與 `controlled-trace-proposal.txt` 給出：

| 相對該次 spawn 前時間 | 原 600ms 政策 | 分類 5 秒提案 |
| --- | --- | --- |
| 約 47–48ms | 進入原生載入 | 進入原生載入 |
| 約 622ms | 中央保存 StartupTimeout | 載入尚在進行 |
| 約 753ms | 未觀察到載入返回／report.begin，最終為強制退出 | 載入返回 |
| 約 754ms | 無 | 開始回報 HookFailed / ConstructModule |
| 約 760ms | 無 | 中央保存分類，Host 收到確認 |
| 最終退出 | signal 9 | code 70，保留 InvalidFactory cause |

因此受控失敗發生在故障回報**之前**，不是 500ms report acknowledgement 等待用完，
也不是已保存的 Plugin 錯誤被 cleanup 或 OSAL 回收覆寫。
同一提案執行中的 60 秒 stall fixture 仍依 600ms startup policy 得到 StartupTimeout，
沒有把真正的啟動停滯放過。

## 調查結束時的修復提案（歷史紀錄）

1. **補足維護中測試的失敗資訊。** 至少輸出 fixture、預期／實際 domain、code、operation、
   cause、MID、ExitStatus、`wait_running()` 結果與 cleanup 診斷。明確區別測試總期限、
   啟動期限與故障確認期限，避免再次只留下泛用斷言。
2. **拆開錯誤分類與啟動逾時的測試設定。** 分類案例使用具名、獨立的有界 setup 預算；
   外部 5 秒提案已取得上述功能證據，仍小於原八秒等待期限。此值是候選值，
   不是正式政策決定或所有環境均足夠的保證。stall 案例保留 600ms，
   最好抽成獨立函式／案例，不從預期錯誤碼暗中選擇政策。
3. **用受控契約補足競爭順序。** 在測試專用 checkpoint 協調「故障先被中央保存」與
   「期限先到、尚未產生故障回報」兩種次序，分別驗證主要故障不被收尾覆寫、
   真實退出／回收及拒絕 Plugin 不安全卸載的防護。維護版應使用有界 gate，
   不把本輪的 700ms sleep 當作精確排程控制，也不在正式 runtime 加測試開關。
4. **同一份測試跨平台驗證。** 不增加 macOS 專用 timeout 分支；Debug／Release
   與 Windows／Linux 使用相同測試結構。先重跑 `module.plugin`、`app.plugin`、
   相依的 Manager／Runtime faults，再恢復完整整合驗收。

不建議改正式 `ModulePolicy` 預設（目前 30 秒）、延長呼叫者 deadline、
讓晚到的 Plugin 故障覆寫 StartupTimeout，或調整 OSAL 的 50ms 回收 fallback。
若另要求「超過啟動期限甚至已被強殺仍必須取得確切 Plugin 錯誤」，那是新的診斷交付契約，
不能由本輪測試修正順帶承諾。現有有界確認／非保證交付政策見
[Module 錯誤與診斷契約](README.md#module-v3-協定)。

## 交付與待辦

本節記錄調查結束時的交付狀態；後續實作狀態見下一節。

- 已完成：非注入新路徑重現、保留完整分類、來源與 fixture hash 核對、
  原生載入時間線、有限慢載入對照、外部測試拆分提案驗證。
- 尚未完成：正式測試修復、維護中的受控契約、修復後完整驗收、Windows／Linux 原生重驗。
- 沒有改正式程式、沒有 benchmark、沒有 commit／push。既有退出修復的未提交工作保留。
- 工作目錄：`/private/tmp/open-vinc-plugin-investigation-20261103`。
  `repeat-results.json`、`path-results.json`、`trace-results.json`、`controlled-results.json`
  記錄每次執行，對應 `.log` 與 `.txt` 保存輸出；`*-commands.json` 保存編譯／連結指令。
  `audit.json` 保存最終來源核對與本案程序清理檢查。證據包位址與 SHA-256 另由交付訊息提供。
- 原始第三步結果包保持不變；本輪結果另外保存，不覆蓋歷史失敗。

## Test repair and acceptance / 測試修復與驗收（2026-11-03）

使用者確認三項驗收要求後，正式維護的測試已完成以下改動；沒有修改 Runtime、
ModuleManager 或 OSAL 的正式實作，也沒有新增公開 API／協定／Plugin ABI。

- `tests/plugin_tests.cpp` 將分類案例的 setup 預算設為具名 5 秒，獨立的
  `expect_stalled_loader()` 保留 600ms。每次公開等待仍為 8 秒；5 秒不是可靠性證明。
  現在驗證 `wait_running()` 的錯誤、中央 snapshot 的 MID／domain／code／operation，
  null factory 的 InvalidFactory cause，以及既有拒絕安全退出與協調器 cause。
  失敗輸出包含 fixture、預期／實際診斷、退出、cleanup 與期限。
- `tests/plugin_test_support.hpp::Scope` 同時持有 Manager 與獨立暫存目錄。
  正常返回與 assertion unwind 都先 `wait_shutdown()`，確認回收後才釋放 Manager、
  刪除 Plugin 副本與 marker。無法確認或清理拋出時，記錄失敗並保留檔案；
  Manager 析構仍執行既有停止／終止程序，不能把析構本身當成刪檔許可。
- 真正的 Plugin 達到 Running 後故意觸發 assertion，離開 Scope 後檢查實際 ExitStatus。
  清理探針還在刪檔前讀取 Plugin 析構寫出的 marker，證明目錄保持至析構完成。
  另一測試由獨立探針確認真實 shutdown，再向 Scope 注入 Timeout 結果，確認它仍保留
  Plugin 與 marker；只有持有獨立回收證據的測試端才移除這些故意保留的檔案。
  這是受控的清理錯誤注入，不宣稱重現了真實 OS 回收失敗。
- `tests/failure_order_tests.cpp` 新增 `module.failure_order`，使用既有
  `VINC_MODULE_TESTING` Manager 編譯副本及 `terminate_process()` seam。
  `failure_order_helper.cpp` 是有 heartbeat 的腳本化生命週期 peer，透過真實控制通道
  送出 primary／cleanup 報告；不是 Plugin loader，也不取代原生 Plugin 測試。
  所有檔案 gate 都有期限，poll sleep 只用來等待狀態，不據此推定事件已發生。

兩種順序都先由控制端 `inspect(mid)` 確認中央已保存指定 MID／domain／code／operation，
之後才釋放下一階段：

| 順序 | 控制方法 | 斷言 |
| --- | --- | --- |
| Plugin 故障先保存 | 允許第一筆 HookFailed 報告，觀察中央 snapshot 後才允許次要報告 | 後來的 ChannelFailed 不覆寫 primary；InvalidFactory cause 保留；cleanup 另存 |
| StartupTimeout 先保存 | 第一筆報告保持在 gate 後，等中央實際判定逾時並要求終止；測試 seam 暫緩 native kill，才釋放晚到報告 | 已保存的 StartupTimeout 不被晚到 HookFailed／ChannelFailed 覆寫；cleanup 另存 |

測試 seam 只延後 native kill 的副作用，不阻塞 supervisor 或持有 record lock 等待。
RAII gate 在任何 unwind 時先恢復真實終止，再由 Scope 回收。第二階段報告均取得對應
request 的中央確認，並再次核對 snapshot；最後讓 peer 真實退出並確認回收。
它驗證的是**中央保存順序**，不是子程序產生錯誤／send 成功的先後。

`module.plugin` 與 `app.plugin` 共用同一組驗證，`app.plugin` 原本的 `RUN_SERIAL` 保留。
獨立目錄另外保護人工或不同 CTest 程序的同時執行。Windows 使用寬字元測試入口，
其餘測試設定三平台相同。新增的 sequential cleanup probes 也有自己的等待上限；
外層 CTest kill guard 統一為 Plugin 180 秒、順序契約 120 秒，為失敗清理預留空間，
不改分類案例 5 秒、stall 600ms 或公開等待 8 秒的判斷。

### 本輪結果

本輪沿用上述 macOS／M1 Pro／Apple Clang 環境，CMake 4.4.4、Python 3.12.14，
獨立 Debug／Release 目錄，完整來源建置啟用 Python/System。只有 Git 外來源副本的
`apps/vinc/vinc.conf` 啟用該設定，repository 設定未改。受測內容為同一基底加既有退出修復
與本輪測試變更；不能歸給 bare HEAD。完整 CTest 依組循序執行，未排除已知失敗。

| 驗證 | Debug | Release |
| --- | --- | --- |
| 完整建置 | 通過 | 通過 |
| 完整 CTest，無略過 | **153/154**，931.59 秒 | **153/154**，934.45 秒 |
| Module／其中四項 Python | 23/23、4/4 | 23/23、4/4 |
| App | 12/12 | 12/12 |
| SDK | 14/14 | 14/14 |
| OSAL | 50/50 | 50/50 |
| `module.plugin`／`module.failure_order`／`app.plugin` | 3/3 | 3/3 |
| 未注入延遲、新路徑 Plugin 副本 | 5/5 | 5/5 |
| Module／App driver 同時執行 | 2/2 | 2/2 |

完整 CTest 唯一失敗均為既有 `examples.user_module` 暫存部署未帶入官方 Plugin／UI 資源，
詳見 [範例部署問題](../osal/MACOS_EXIT_VALIDATION.md#example-deployment--範例部署)。
本輪未修改該 fixture，也未將它排除，**不能宣稱全專案全綠**。
其後使用者另行授權的 [範例部署修復](../../../examples/userExampleModule/README.md#macos-deployment-fixture-gap)
已通過 macOS Debug／Release 定點補驗；不是本報告的完整 CTest 重跑。
Debug 先行的 Manager／Runtime faults、Plugin／順序／App Plugin 五項亦通過，
最終兩組完整測試包含最後的 helper 依賴修正。
新路徑副本逐檔核對 SHA-256；這不是受控 OS 冷快取。並行組直接啟動兩個 driver，
只繞過 CTest 排程，保留所有斷言，用來檢查獨立暫存目錄的隔離。

新增三個 Git 外的 Debug 反向測試，逐一破壞受驗契約；都以預期斷言退出（code 1），
不是只觀察任意失敗或外層逾時：

| 刻意破壞的行為 | 偵測結果 |
| --- | --- |
| Manager 讓後來的主要故障覆寫已保存紀錄 | `later primary did not replace central primary` |
| Scope 在 shutdown 前先刪除目錄 | `assertion cleanup keeps marker directory until Plugin destructor finishes` |
| Scope 在未確認 shutdown 的分支刪檔 | `uncertain cleanup retains Plugin and marker` |

反向測試只改外部副本，不修改受測正式來源；失敗 unwind 仍恢復真實終止並回收。
因此中央順序與清理契約有獨立證據，不靠 5 秒預算或重跑次數聲稱可靠。
中途 helper 引入具名退出常數後曾缺少 target 的 header 依賴，已補上 `vinc::module`，
失敗 log 仍保留；最後完整建置通過。既有 compiler／linker warnings 並未宣稱清零。

原始調查與前一輪完整 CTest／證據包均未覆寫。Windows／Linux 原生驗證仍待補，
沒有 Intel Mac、最低 macOS 或其他工具鏈的支援保證；本輪不做 benchmark，也不 commit／push。
最終核對 repository 與受測快照的非文件內容一致（上述外部設定差異除外），
正式 Manager 未帶入 `VINC_MODULE_TESTING`；新增三個測試編譯均為 C++23。
`git diff --check` 通過，所有 runner 已結束，本案工作路徑的唯讀程序篩選沒有殘留。

### 交付紀錄

- 工作目錄：`/private/tmp/open-vinc-plugin-repair-20261103`。
- 結果包：`/Users/jay/Downloads/open-vinc-plugin-repair-20261103/evidence.tar.gz`，
  同目錄 `SHA256SUMS` 及 `archive-verification.json` 提供壓縮檔與逐檔驗證結果。
- 保存環境／dependency pins、受測來源 hash、完整與巢狀 logs、JUnit、
  新路徑 fixture hashes、反向測試來源與命令、最終來源／程序稽核及重現說明。
  不含 build binaries、上游依賴樹或 Git metadata。
- 原始證據留在 Git 外；repository 維護測試、報告與 TODO，不提交原始測試結果包。

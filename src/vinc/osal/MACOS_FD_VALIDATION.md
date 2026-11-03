# macOS FD／spawn 驗收與退出觀察調查（2026-11-03）

測試版本：`a19c0d456efa8f4ad95f4570e8b43f1b6daf1e00`，PR #22 合併後的 `develop`。
本輪分支：`codex/macos-fd-acceptance-20261103`。與修復提交 `49cbfea` 比較，
非 Markdown 檔案沒有差異。本輪僅執行測試、診斷與更新文件，沒有修正正式程式。
共同契約仍由 [FD／spawn 計畫](FD_SPAWN_PLAN.md) 維護。

**三階段測試與調查已完成；一般驗收通過，但退出觀察缺口仍未修復。**
未修改版控制組仍可重現逾時，不能將本輪稱為所有程序契約全綠。

後續更新：上述是 `a19c0d4` 當時的結果。退出修復第二步已完成實作與針對性契約測試，
詳見 [退出進度紀錄](PROCESS_EXIT_PROGRESS.md)；修復後 macOS 原生退出／整合結果由
[第三步報告](MACOS_EXIT_VALIDATION.md)維護，包含保留的整合失敗。Windows／Linux 回歸仍待補。

## 環境與證據方法

| 項目 | 環境 |
| --- | --- |
| OS／CPU | macOS 26.3.1(a)，build 25D771280a；Apple M1 Pro arm64 |
| C++／標準庫 | Apple Clang 21.0.0（clang-2100.1.1.101）；libc++ 210106；C++23 |
| SDK／工具 | macOS SDK 26.5；CMake 4.4.4；Ninja 1.13.2.git.kitware.jobserver-pipe-1 |
| Python／Node | 測試與下游 CPython 3.12.14；外部 supervisor 使用 Apple Python 3.9.6；Node 24.22.0 |

從追蹤檔建立 Git 外的來源快照，768 個一般檔案逐一記錄 SHA-256。
固定版本依賴及既有 `node_modules` 以連結重用。來源組態使用未更動的預設 C++ App，
沒有啟用 bundled App 的 Python；installed Python／多 App 下游仍使用選定的 CPython。
主驗收設 `PYTHONMALLOC=debug`、`PYTHONFAULTHANDLER=1`，CTest 序列執行。
建置使用獨立的 Debug／Release 目錄，不重用前次版本的 binaries。

獨立 OSAL 每組態 57 個 C++ 編譯命令、來源全套 290 個、乾淨 SDK producer 92 個。
全部使用 `-std=c++23`；Debug 使用 `-g` 且沒有 `NDEBUG`，Release 使用 `NDEBUG`。
主來源與乾淨 producer 都沒有編譯 Python adapter；下游才依 App 選擇建立 binding。
外部受控／退出診斷與部分建置、整合測試在同一主機重疊執行，沒有做效能隔離。
下列次數是此環境的功能證據，**不是自然故障率或 benchmark**。

## 第一階段：底層與 FD 行為

原始、未改測試的獨立 Common／OSAL CTest：**Debug 42/42、Release 42/42**，沒有略過。
包含 `osal.coordinator`、`osal.coordinator_shared`、`osal.coordinator_late`、
八組 `osal.fd_spawn.*`、`osal.descriptor` 及既有程序／Pipe／Socket／通知回歸。

維護中的 FD 測試涵蓋 512 種 present／CLOEXEC／Inherit-or-Null 組合、
快照後更換來源 FD、三個複製位置的真實 EMFILE、重試與 native spawn 失敗回收、
producer 接線、實際 Socket 建立後拒絕晚綁定、accept 的 try／期限／取消，以及
1／63／64 個 Pipe handoff。`osal.descriptor` 另驗 FileLock owner 退出後的鎖回收。
這不等於 FileLock 的原生 unlock 失敗政策已解決。

### 前次證據工具的修復後對照

前次工具來自既有外部 FD 結果包，改寫後仍只保存在 Git 外。
未插入觀測點的 Common／OSAL 副本與本輪來源逐檔相同。
受控副本只有 relocation、acquire 前與 stdio actions 完成的觀測點；保留 errno，
不替換 native 回傳值或改變資源所有權。

新排程先讓資源建立停在低 FD 尚未搬移處，確認 spawn 已嘗試取得協調，
再以 `ResourceGuard::try_acquire()` 確認 producer 持鎖且 spawn 尚未完成 actions。
接著放行搬移，才等待 spawn actions。原本的修復前排程不能原封不動沿用，
否則會要求兩個互斥操作同時進入而製造 fixture deadlock。

| 外部對照，每組態 | Debug 完整案例通過 | Release 完整案例通過 | FD 行為判讀 |
| --- | --- | --- | --- |
| 缺少標準串流 × Inherit／Null／Capture | 143/144；1 次退出觀察逾時 | 142/144；2 次退出觀察逾時 | 各 144/144 的 FD／資料斷言通過 |
| 同矩陣，保留串流設 CLOEXEC | 143/144；1 次退出觀察逾時 | 143/144；1 次退出觀察逾時 | 各 144/144 的 FD／資料斷言通過 |
| 1／63／64 Pipe bindings 與 capture | 24/24 | 24/24 | 內容、映射與 EOF 通過 |
| 資源／排程矩陣，含雙 spawn 與 EOF | 修正外部 fixture 後 213/213 | 211/213；2 次退出觀察逾時 | 最終各 213/213 的 FD 斷言通過 |
| Notification keepalive 端點排程 | 6/6 | 6/6 | create/open 的第三次低 FD 取得另驗 |
| 重複 FileLock 排程 | 100/100 | 100/100 | 通過 |
| 固定 seed 的資源排程抽樣 | 100/100 | 100/100 | 通過；seed 20261102 |

完整案例通過必須包含正常退出觀察；不因 FD 斷言通過或後續 `try_wait()` 救回而抹去逾時。
`probe-independent-assessment.json` 對原始結果補做完整資料判讀，保留原分類，
避免舊 oracle 遇到退出逾時便提早返回、未檢查其餘資料。
另外在四份隔離的觀測資料副本，分別移除排他證據、改成延後 EOF、改錯第一與第二個
child 的資源身分，判讀工具均拒絕。這是 oracle 的合成負向檢查，不是正式程式故障注入。

資源矩陣涵蓋 FileLock create/open、SharedMemory create/open、Pipe、Socket/accept、
三種 monitor、Notification directory／endpoint。驗證子程序繼承的是正確資源或
缺少串流的 `/dev/null`，沒有額外 FD；Pipe 的非預期 writer 不再延後 EOF。
EOF 案例在 child 仍存活時取得 EndOfStream，再終止並回收 child。
原始來源快照後的 FD 重用由維護中的 `osal.fd_spawn.reuse` 另行驗證。

最初 Debug 資源矩陣有 212 個通過及 1 個 **oracle failure**：修復後 EOF 提早抵達，
外部工具太早終止 linger child，尚未讀到完整身分報告。只在外部工具加入有界等待，
確認 child 已寫出報告，再驗 EOF／存活與終止。保留初次紀錄，完整重跑 213/213 通過；
Release 使用已修正工具。這不是 Runtime 修正，也不把初次 213 個都記為成功。

前次未修復版本 CLOEXEC 各 69/144 失敗的串流行為，本次對照未再出現。
但是退出觀察缺陷仍存在，因此本輪不能宣稱所有程序契約全綠。

## 第二階段：App／Plugin／乾淨 SDK

| 驗證 | Debug | Release |
| --- | --- | --- |
| 全新來源建置／公開標頭編譯 | 通過 | 通過 |
| 原始完整 CTest | **132/132**；912.17 秒（driver） | **132/132**；929.46 秒（driver） |
| 乾淨 SDK producer，關閉 App／Python | 通過，92 個 C++ 編譯命令 | 通過，92 個 C++ 編譯命令 |
| 額外乾淨搬移 SDK 原生 consumer | 4/4 | 4/4 |
| 額外乾淨搬移 Application | 通過 | 通過 |

兩組原始 CTest 沒有失敗或略過。完整套件包含 `module.plugin_initialize`、
`module.plugin`、`app.plugin`，驗證 entry 11、舊 entry 10／缺入口／建構失敗／
初始化拒絕／過晚綁定、故障隔離與清理；另有 Context、Bus、Param、RPC、Shell、
重啟與 system services 回歸。

`sdk.installed_consumer` 及額外乾淨 consumer 都用搬移 SDK 的 targets，
重新編譯一個執行檔與兩個獨立靜態連結 OSAL 的動態庫；驗證共享 owner、互斥、
期限／取消、錯誤／晚綁定與卸載生命週期。其四個 nested tests 不再累加到 132 個頂層數字。

`sdk.installed_application` 與額外乾淨 Application 驗證搬移、暫藏 SDK／build inputs、
不同 cwd、不同 Host PID 的自訂巢狀訊息 copied／loaned 通訊、Recorder 持久化、
UI HTTP preview、schema 增量變更與拒絕／修復；部署 inventory 排除額外 Host／測試程式。
來源 `sdk.external_consumer`、installed `sdk.multi_app`／`sdk.system_apps` 以及
`sdk.installed_python` 亦在兩組完整 CTest 中通過。

這裡的完整套件是本輪 C++ producer 設定實際登記的 132 個案例。
UI 驗證是 HTTP，不宣稱互動 browser／GUI 驗證；沒有因退出調查而改動測試期限。

與上一份 [App／Python／SDK 報告](../sdk/MACOS_VALIDATION.md) 的來源與組態不同。
本輪 bundled App 未啟用 Python，所以不包含已知的 `examples.user_module` 漏部署資源案例；
該 fixture 尚未修正，不能以這輪測試結果結案，也不是刻意從已登記 suite 排除失敗。

## 第三階段：退出觀察的重現與定位

每個案例建立新的 fixture 程序、新的 ProcessService／PipeService，啟動一個正常
退出的子程序；不強迫 FD 排程，不加入額外 reaper。仍用既有 5 秒 `wait_until()`。
supervisor 對超過 0.25 秒的案例以 `ps` 保存父子狀態；它不呼叫 waitpid 回收 OSAL child。
逾時後由原 OSAL `try_wait()` 嘗試取得結果，保留失敗後再清理，沒有提高公開等待期限。

| 觀測方式，每組態 3,000 次 | Debug | Release |
| --- | --- | --- |
| 未修改 Common／OSAL | 2,996 通過、**4 逾時** | 2,999 通過、**1 逾時** |
| 完整時序追蹤副本 | 3,000 通過 | 2,999 通過、**1 逾時** |
| 輕量追蹤副本 | 3,000 通過 | 2,995 通過、**5 逾時** |

三種方式合計 18,000 次，11 個逾時樣本全部有 zombie 快照，且 `try_wait()`
取得正常 exit code 0。完整追蹤影響排程，不能以 Debug 追蹤版未重現撤銷原版失敗。
兩種追蹤都使用有界記憶體紀錄；完整版保存 watch／record publication／wake／kevent／
waitpid／waiter 的先後，輕量版只保存最近 NOTE_EXIT PID，並在真實 waitpid 返回後紀錄。
所有觀測點及兩種工具來源保存在外部，正式程式不包含它們。
trace 時間為單調奈秒。`watch_end`／`wake_end` 的 errno 欄只在原生回傳失敗時有意義；
成功呼叫留下的非零 errno 不是失敗證據。waitpid 的返回 PID／0／-1 才決定如何解讀該次結果。

### 已確認的失敗路徑

代表樣本：`exit-results/Release-trace/02636/trace-before-recovery.txt`。

1. PID 35228 的 watch 註冊成功，record 已發布；啟動與 wait 的 user wake 都有送達。
2. observer 收到 `EVFILT_PROC / NOTE_EXIT`，PID 正確；沒有紀錄截斷。
3. 接著呼叫真實 `waitpid(35228, ..., WNOHANG)`，**返回 0**。
4. `observe()` 因尚無結果而返回，record 仍被視為 event-driven。
5. `run_monitor()` 再次進入 `kevent(..., timeout=nullptr)`；一次性 watch 的退出事件已消耗。
6. child 在約 0.25 秒的快照已是 zombie，但 waiter 到 5 秒仍逾時。
7. `try_wait()` 再次進入同一 OSAL 觀察路徑，waitpid 返回該 PID／exit 0，才發布退出結果。

五個 Release 輕量追蹤逾時樣本亦全部記錄到「收到同 PID 的 NOTE_EXIT 後，waitpid 返回 0」。
完整追蹤排除了此代表樣本的 watch 註冊失敗、record 尚未發布、user wake 未送達，以及
事件先被其他 reaper 偷走等解釋。它不代表所有歷史失敗都已逐一取得相同 trace。

**已確認的實作缺口：一次退出通知之後，若非阻塞回收尚未得到結果，現有狀態機沒有
保留再次觀察的進度保證。** 對應位置為 `src/macos/monitor.cpp::NativeMonitor::wait`
與 `watch_process`（EV_ONESHOT）、`src/posix/process.cpp::NativeProcess::poll`，以及
`src/process.cpp::{observe,run_monitor}`。這是本次實測定位，並非已完成修復。

程式碼上的原因鏈也相符：`event_driven()` 仍回傳最初 watch 註冊成功留下的
`monitored=true`；這不反映一次性事件已消耗。poll 的空結果既不設定 observation error，
也不改變該旗標，所以 `run_monitor()` 不會啟用既有 fallback。
目前 `NativeMonitor::wait()` 只回傳等待成功，沒有把退出事件的後續觀察責任交給 record。

### 後續修改方向（提案，未實作）

- 保留「已收到退出事件、尚未確認回收」的狀態；此時不能直接依賴已消耗的一次性事件
  再做無期限等待。評估只針對這類 pending exit 安排有界後續觀察，或可證明可靠的原生再通知。
- 不直接在共用 observer 改用阻塞 waitpid；必須先確認對其他 child、取消及 shutdown 的影響。
  是否沿用既有 fallback、採用何種重試節奏與放在哪一層，需下一輪設計 review。
- 保留 OSAL 唯一 reaper、record 鎖與真實 ExitStatus；不能憑 NOTE_EXIT 就虛構成功、
  提前回收上層共享資源，或要求應用程式自行定期 `try_wait()` 補救。
- 下一輪契約測試應受控重現「退出事件已消耗，但第一次 poll 無結果」，驗證無其他外部
  API 呼叫時仍能完成觀察；同時覆蓋多 child、公用 waiter、取消、期限、shutdown 與資源回收。
  再重跑未插入觀測點的 Debug／Release 控制組及三平台回歸。

這些提案不構成實作授權，本輪未加入輪詢或更改公開期限。

## 重現、交付與限制

正式測試可依 [第二階段](FD_SPAWN_PLAN.md#stage-2-implementation) 與
[整合驗收](FD_SPAWN_PLAN.md#stage-4-acceptance) 的 CMake／CTest 指令，
在本文件版本分別建立 Debug／Release。不要以只重跑失敗項目取代原始整輪紀錄。

本機完整建置與原始工作目錄：`/private/tmp/open-vinc-macos-fd-20261103`（暫存）。
持久交付目錄：`/Users/jay/Downloads/open-vinc-macos-fd-acceptance-a19c0d4-20261103/`。
`evidence.tar.gz` 保存來源快照、工具、原始紀錄與報告；`SHA256SUMS.json` 逐檔列雜湊，
`export-verification.json` 記錄封裝後逐檔驗證、檔案數、大小與壓縮檔 SHA-256。
不含可重建 binaries、SDK payload 或依賴 symlink 目標；其他主機須另外取得此本機包。
環境／來源雜湊在 `environment.json`、`source-sha256.json`；實際命令及退出碼在
`results.json`、`probe-driver-results.json`、`probe-corrected-driver-results.json`、
`exit-driver-results.json`、`light-driver-results.json` 與 `clean-sdk-*-results.json`。
driver 正常完成不代表每項測試成功；JUnit、逐案例 assessment 與 trace 才是各項結果。

`probe-results/` 保留所有受控／串流案例；`exit-results/` 保留 18,000 次正常啟停的
個別資料及失敗時的程序快照。`probes/` 內有原版、受控、完整／輕量追蹤的分離來源、
instrumentation patch 與初次／修正版工具。建置產物及原始資料不加入 Git。

全部測試結束後，以本輪工作目錄為範圍的唯讀程序盤點確認沒有殘留程序，見
`cleanup.json`；紀錄不保存程序命令列。此結果僅限本輪收尾觀察，不構成通用資源洩漏保證。

- 沒有 benchmark、Intel Mac、舊 macOS、其他 Clang／libc++ 或高程序數效能證據。
- 此報告不替代 Windows／Linux；也不承諾任意第三方 native FD 操作受協調。
- 外部工具的 FD 集合檢查掃描 3–511；本輪控制組即時及有界最終檢查均無差異。
  這不構成通用 leak-free 證明，亦不把 `shutdown_until()` 升格為 thread join／安全卸載屏障。
- 外部工具允許在所有 public views 放掉後觀察最多 1 秒的資源收尾；沒有放寬 child wait。
- 舊範例部署 fixture、FileLock unlock 失敗政策與 Socket Shell 延後項目均未修改。

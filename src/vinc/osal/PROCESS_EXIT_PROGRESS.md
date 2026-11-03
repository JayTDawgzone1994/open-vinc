# Process exit progress: controlled reproduction and lifecycle design

## Status / 狀態（2026-11-03）

本文件維護 macOS 一次性退出通知之後的內部生命週期修復設計與受控重現。
原生缺陷證據由 [Mac 驗收報告](MACOS_FD_VALIDATION.md)維護；整體進度與已確認的
FD 契約由 [FD／spawn 計畫](FD_SPAWN_PLAN.md)維護。

**第二步實作、第三步 macOS 驗收及後續兩項測試修復已完成；原始失敗紀錄保留。**
使用者後續明確授權第三步；原生退出、完整 App／Python／乾淨 SDK 矩陣、保留的失敗
與交付見 [第三步驗收](MACOS_EXIT_VALIDATION.md)。後續 Plugin／範例修復的不同驗證輪次
統一索引於 [目前驗收狀態](MACOS_EXIT_VALIDATION.md#current-acceptance-status)，不改寫原始全套結果。
第四步 Windows／Linux 修復後回歸尚未執行，不以 Mac 結果代替。
沒有 benchmark。第一步的失敗紀錄保留為歷史證據，不改記成修復後的成功。

### Confirmed constraints / 已確認約束

- 待回收狀態對應正確程序紀錄；舊 PID、事件或物件位址不能誤認新程序。
- 取消與逾時只結束該次等待；保留背景觀察與其他等待者取得真正 ExitStatus 的能力。
- 只有需要補查時才啟用既有 50ms fallback；不延長 caller deadline，不永久輪詢所有程序。
- 保留同一個 OSAL 回收路徑、既有 record 鎖、所有權與錯誤契約；不新增執行緒、
  程序全域 signal handler、外部 reaper 或供應用自行補救的要求。
- 受控重現必須消耗唯一通知、令第一次 poll 返回空結果，之後不靠公開 API 或額外事件救回。

## Step 1 evidence / 第一階段證據

基準正式程式：`a19c0d456efa8f4ad95f4570e8b43f1b6daf1e00`。
本步只新增診斷來源、測試 CMake target 與文件；`src/`、`include/` 的正式 OSAL 沒有改動。
使用 macOS 26.3.1(a) / 25D771280a、Apple M1 Pro arm64、Apple Clang 21.0.0、
libc++ 210106、SDK 26.5、CMake 4.4.4、Ninja 1.13.2.git.kitware.jobserver-pipe-1。
Debug／Release 分別建置，C++23；外部執行工具使用 Apple Python 3.9.6。
建置成功；原有 Pipe aggregate initializer 與 `posix_spawn_file_actions_addchdir_np`
棄用警告仍保留於 build logs，本步未修改那些實作。新診斷來源沒有編譯警告。

### Controlled observer diagnostic / 受控 observer 診斷

第一步來源 `process_exit_progress_probe.cpp` 保存在下方第一步結果包；第二步已由
[正式契約測試](tests/process_exit_progress_tests.cpp)取代。
它編譯未修改的 `src/process.cpp`，以連結期的假 native boundary 驅動真正的共用
observer。沒有建立 OS child，也沒有模擬或替換正式 macOS syscall；原生時序另見前份報告。

每次執行建立新的服務及一個程序紀錄，先啟動公開 `wait_until()`，確認 spawn 和 waiter
初始 wake 都被 observer 消耗。之後只送一次合成退出通知。第一個後續 poll 返回空結果，
fixture 同時讓下一次 poll 可以取得 exit code 42；不再送事件，不呼叫 `try_wait()`。

基準 monitor 的非 polling 等待使用有 predicate 的 condition variable 無期限等待；
spurious wake 不能讓它返回 observer。公開等待期限為 600ms，fixture 協調／清理期限為
5 秒，外部 driver 每個程序最多 12 秒。協調未完成就逾時會算 fixture failure，不能當成重現。

| 模式，每組態 30 次 | Debug | Release | 解讀 |
| --- | --- | --- | --- |
| `one-shot` | 30 次退出碼 **1** | 30 次退出碼 **1** | 契約失敗：一次空 poll 後回到無期限等待，公開 wait 逾時 |
| `retry-control` | 30/30，退出碼 0 | 30/30，退出碼 0 | 假 native 在空 poll 後回報非 event-driven，真正 observer 使用既有 50ms fallback 完成 |
| `immediate-control` | 30/30，退出碼 0 | 30/30，退出碼 0 | 第一個事件後的 poll 直接取得退出結果，不需要 fallback |
| `periodic-control` | 30/30，退出碼 0 | 30/30，退出碼 0 | 假 monitor 即使 polling=false 仍每 5ms 返回，證明這種 fixture 會掩蓋缺陷 |

基準每次都確認 event_count=1、empty_after_event=1、exit_results=0、polling_waits=0、
timed_returns=0，且空 poll 後確實進入非 polling 等待。四種模式均確認事件後沒有額外公開
wake 或 waiter poll。先固定結果與統計，再用明確標記的 cleanup wake 讓 worker 完成，
執行 shutdown／forget，最後確認 monitor 已解構。所有 240 次都有 cleanup_complete=true。
這不是把逾時後的補救算成正常成功。

退出碼 1 保留為失敗；沒有使用 CTest WILL_FAIL 或把預期的缺陷包裝成契約通過。
退出碼 2 是 fixture／用法失敗，不能當成已重現。錯誤模式參數另驗兩組都返回 2。
現有 `osal.process_faults` Debug／Release 各 1/1 通過，但其原本 5ms 假等待不構成本缺陷的驗證。

當時診斷 target 使用 `EXCLUDE_FROM_ALL`，未加入預設 CTest inventory。
它對原錯誤路徑的斷言已在第二步改成成功契約，並接入預設 CTest；原始失敗來源留在外部包。

### Native token transport / 原生代號傳遞探測

本機 SDK 的 `kevent64_s::udata` 是 `uint64_t`。額外使用原生 `kevent64`、
`EVFILT_PROC / NOTE_EXIT / EV_ONESHOT` 與受管制的真實子程序驗證完整 64-bit 代號：
先 dequeue 再 reap、先 reap 再 dequeue、與 user wake 共存，各 20 次，共 **60/60**。
代號使用高位非零的值，以免只有低 32-bit 正確卻被誤認完整傳遞。

這輪「先 reap 再 dequeue」的 20 次仍取到帶有舊代號的事件；因此回收完成後仍可能遇到
舊事件，不可依事件中的 PID 找目前 owner，也不可解參考過期物件指標。
即使某個環境不再提供此舊事件，也不影響設計：已完成紀錄不再需要退出進度。
已取出的事件是數值副本，回收 child 後仍可安全檢查。

在設計 review 中進一步檢查註冊端，發現只傳 udata 還不夠：必須讓它參與原生 watch
識別。本機 SDK 的 `kqueue(2)` 說明與 [Apple XNU 的 watch 比對實作](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_event.c)
均區分一般 `(ident, filter)` 與 `EV_UDATA_SPECIFIC` 的 `(ident, filter, udata)`。
上游原始碼僅作設計參考，不宣稱等同這台 Mac 的精確 kernel 版本。

額外原生對照（每項 20 次）：

| 操作 | 實際結果 | 設計意義 |
| --- | --- | --- |
| 舊 child 已 reap、事件未取出；沒有 specific flag，以相同 PID 註冊新代號 | 20/20 註冊成功，舊事件改帶新代號 | 只使用 udata 值仍不足以保護身分 |
| 相同操作，加上 `EV_UDATA_SPECIFIC` | 20/20 新註冊返回 ESRCH，舊事件保留原代號 | 新代號不會改寫既有 watch；此處沒有真的建立重用 PID 的新 child |
| 同一個存活 child，以 specific flag 註冊兩個代號再讓它退出 | 20/20 分別收到兩個代號 | 驗證同 ident/filter 的兩份 watch 可依代號分開 |

再將前述 60 個傳遞案例全部加上 specific flag 重跑，60/60 符合預期。
合計 **180 個原生案例**；其中無 specific flag 的重綁對照刻意證明不安全方案，不能記成
該方案通過安全契約。第二步採 `EV_ADD | EV_ONESHOT | EV_UDATA_SPECIFIC`。

來源與完整輸出在外部結果包的 `native_token_probe.py`、`native_token_probe_specific.py`、
`native_token_identity_probe.py` 及各自的 `native-token-*-results.json`。
它們沒有驗證新 observer、真正 PID 重用、Intel Mac 或最低支援 OS；不視為正式修復驗收。

## Internal design / 內部設計（第二步已實作）

### Identity and event transport / 身分與事件傳遞

1. 重用已在 native spawn **之前**分配的 `ProcessId` 作事件代號。`next_id()` 在同一
   OSAL image 內不重用且耗盡時拒絕；monitor 僅服務其所屬 service，不跨 image 分派。
   不新增 PID registry、序號產生器或借用物件指標。
2. 私有 `NativeProcess::spawn` 接收該代號，POSIX 的 `watch_process` 將其傳到 Mac backend。
   Mac process monitor 統一採 `kevent64`／`EV_SET64`，將 `ProcessId.value` 寫入整數 udata，
   並以 `EV_UDATA_SPECIFIC` 讓代號參與原生 watch 識別，避免同 PID 的註冊覆寫舊事件；
   不用整數轉 pointer 或把 C++ object address 放進事件。其他 monitor 不納入此改動。
3. 私有 `NativeMonitor::wait` 成功時回傳固定容量的事件批次，包含已消耗的退出代號。
   容量對齊既有每次最多 32 個原生事件；不用動態配置。只轉送 `EVFILT_PROC / NOTE_EXIT`，
   user wake 只喚醒掃描。超過一批的事件留在原生佇列供後續 wait 取得。
4. Linux／Windows 保留現有通知與回收機制，成功 wait 回傳空的代號批次，讓共用 observer
   照常掃描。這是 OSAL 私有介面調整，公開 API、Module 協定與 Plugin entry 不變。
   三平台與測試 native implementations 都必須適配並重建。

批次內若遇 native error，保留既有 `Result` 與 `monitor_error` 診斷處理。
若返回 error 而未交付同批已消耗的退出代號，共用 observer **必須仍進入現有的持續 fallback**，
不能丟掉退出事件後又恢復無期限等待；此混合事件／錯誤情境須受控回歸。
不要為了完整批次而吞掉 native error，也不要把尚無結果的 poll 包裝成錯誤。

### Record state and ordering / 紀錄狀態與順序

在既有 `ProcessRecord` 增加內部 `exit_notification_pending` 狀態，由 record mutex 保護。
它表示一次退出通知已消耗而退出結果尚未確認，不等於已退出、不授權回收上層共享占用。

每輪 observer 在下一次 poll **之前**處理剛取得的事件批次：先持有 service mutex，
再依既有順序取得 record mutex，以 `ProcessId` 精確匹配。

| 事件對應紀錄的狀態 | 動作 |
| --- | --- |
| `starting=true` | 保存 pending；不要因 native 尚未發布就丟棄通知 |
| native owner 存在、沒有退出結果 | 保存 pending，透過原 `observe()` 嘗試回收 |
| 已有 ExitStatus，或已失去 owner | 忽略舊通知；不能重新啟用 pending |
| 紀錄已移除／代號不匹配 | 忽略；不能退回用 PID 或位址比對 |

既有 spawn 流程先把 starting record 放進 service，再做 native spawn/watch，最後發布
native owner 並 wake。若事件先到，pending 留在 starting record，發布時不得覆蓋它。
starting 階段仍依發布 wake 推進，不因尚無 native 就把 pending 當作已完成。
若 spawn 失敗而移除 record，之後收到其事件只會找不到代號，不能污染另一個 record。

poll 回空結果時保留 pending。仍有 native owner 且 pending 時，加入本輪 polling 判斷；
既有未註冊成功／觀察錯誤的 fallback 條件仍保留。真正取得 ExitStatus 或確認
ObservationLost 並釋放 owner 時，在共用 `observe()` 清除 pending，涵蓋 worker 與公開操作。
一般觀察錯誤保留 owner、pending 與首次錯誤診斷，沿用原有錯誤退避政策。

目前 observer 每輪掃描所有有界紀錄。第一版保留這個做法：有待回收 owner 才啟用補查，
但被喚醒的那輪仍掃描其他紀錄；不承諾只 poll 單一程序。所有補查理由消失後恢復事件等待。
50ms 是檢查間隔，不是退出完成上限；排程與 native 行為仍可增加延遲。

### Lifetime and lock proof / 生命週期與鎖順序

- 原生事件只持有整數代號；不保有 record、NativeProcess 或 service 指標。
  原生 watch 移除與已取得批次的存活時間不需要延長 C++ object lifetime。
- 共用 observer 在 service → record 鎖下匹配及修改。公開操作仍只拿 record 鎖，
  不新增 record → service 的反向取得，也不在 native wait 期間持有這些鎖。
- 若公開操作先回收，事件處理看到無 owner／已有結果而忽略；若事件先標記，公開
  `observe()` 取得結果時清掉 pending。兩者對同一 record 由同一把鎖序列化。
- 舊事件即使攜帶已重用的 PID，也帶著舊 `ProcessId`。新紀錄使用不同代號，不能匹配。
  保留既有 Mac 不在 reap 後依 PID 刪除 watch 的規則，避免刪掉新程序的 watch。
- callback 不進入應用；不新增 observer 執行緒、lock-free 容器、等待中的配置或
  spawn 成功後可能拋出配置失敗的新步驟。新 record 狀態在既有 pre-spawn 配置內完成。
- caller 取消／逾時不修改 pending、owner 或其他 waiter 的結果。service 關閉後仍保留
  背景責任直到觀察完成；不把 `shutdown_until()` 擴張成 thread join／安全卸載屏障。

## Step 2 acceptance gates / 第二步必要驗收

- 受控消耗唯一通知，第一次及連續數次 poll 無結果，之後不靠任何額外事件也能完成。
  fallback 期間不 busy-spin；完成後回到非 polling 等待。
- 事件早於 native publication；事件已在本地批次而公開操作先回收；紀錄移除後舊事件抵達。
- 受控重用同一 PID／不同 ProcessId；舊事件不能讓新紀錄進入 pending 或取得舊結果。
  不需要靠壓力測試碰巧等到 OS 真正重用 PID。
- 多 child、同批多事件／超過一批、重複事件、user wake 與退出事件共存，以及混合 native error。
- 多 waiter，其中一個取消／逾時；已返回的 waiter 不放棄背景回收，另一個仍取得相同真實結果。
- poll error 保留所有權／診斷與後續恢復；ObservationLost 清理 pending 並拒絕再控制舊 PID。
- shutdown、釋放 public view、forget、記錄容量回收，以及正常沒有退出事件時不永久輪詢。

第一步的 fake-backend 控制只證明原失敗路徑與 fallback 因果。第二步已將上述要求接入
正式受控測試，結果如下。第三步仍需完整原生 macOS／整合驗收，第四步由 Windows／Linux
在同一提交補驗；不能將本機針對性通過稱為最終三平台驗收。

## Step 2 implementation and validation / 第二步實作與驗證

驗證輸入為 `a19c0d4` 加本輪未提交的修復、測試及文件；逐檔 SHA-256 隨外部結果包保存。
環境與第一步相同。沒有公開 API、Module 協定、Plugin entry、worker 數量或 caller deadline 變更。

正式變更位置：

- [私有 native 介面](include/vinc/osal/detail/native.hpp)：固定容量 `ProcessExitEvents` 批次，
  spawn 傳入既有 ProcessId。
- [共用 observer](src/process.cpp)：starting／active 紀錄保留 pending，唯讀事件身分比對在
  service → record 鎖內完成；原 `observe()` 在取得結果或失去 owner 時清除 pending。
- [Mac monitor](src/macos/monitor.cpp)：`kevent64`、整數 udata 與 `EV_UDATA_SPECIFIC`，
  將已消耗的 NOTE_EXIT 身分送回 observer；混合 error 沿用既有錯誤 fallback。
- [POSIX spawn](src/posix/process.cpp)、[Linux monitor](src/linux/monitor.cpp)、
  [Windows native backend](src/windows/process.cpp)：接上私有簽名；後兩者保留原通知／回收機制，
  回傳空事件批次。Linux／Windows 本輪未原生編譯執行，需第四步補驗。

### Maintained contracts / 已接入 CTest 的契約

[共用狀態機測試](tests/process_exit_progress_tests.cpp)編譯真正 `process.cpp`，以受控 native
boundary 注入事件與 poll 結果。event-driven 一律為 true；非 polling 的 wait 沒有 timer。
核心進度案例在證明背景已完成回收前不呼叫任何公開 wait／try_wait，不用額外 wake 救回。

| CTest 後綴 `osal.process_exit_progress.*` | 驗證 |
| --- | --- |
| `one-shot`、`repeated-empty`、`immediate` | 一次通知後 1／3／0 次空結果；取得真實結果；補查次數符合空結果次數，完成後停止補查 |
| `early-publication`、`failed-publication` | 通知早於 native 發布；發布失敗移除紀錄，容量可重用，舊通知不影響新紀錄 |
| `stale-retained`、`stale-forgotten` | 事件已取出但公開 wait 先回收；保留／刪除舊紀錄後，相同 PID 的新 ProcessId 不受舊事件影響 |
| `batches` | 40 個 child、兩批事件、重複及未知代號、user wake 共存，結果不遺失、不重複回收 |
| `waiters` | 三個等待者；一個取消、一個在 pending 期間逾時，另一個仍取得結果，不放棄背景 owner |
| `monitor-error`、`poll-error` | 消耗事件後 monitor 回 error 仍補查；一般 poll error 保留 owner、首次診斷與後續恢復 |
| `ownership-lost` | 失去 owner 後不假造退出、不再 signal 舊 PID；後到事件不重新啟用補查 |
| `shutdown` | 釋放公開 view、shutdown 逾時後仍回收；保留結果並可 forget |
| `record-state` | 在 record 鎖下直接檢查空 poll／一般錯誤保留 pending，成功／失去 owner 清除 pending，首次診斷保留 |

這些測試的 5 秒是 fixture／結果等待上限；polling 間隔仍為 50ms，idle 觀察窗 130ms，
取消／逾時案例的短期限為 250ms，CTest 每案例上限 20 秒。以 predicate 控制排程，
先驗結果再清理；末尾確認 monitor 解構。不把 shutdown 的現有公開契約改成 thread join。

[Mac backend 測試](tests/process_exit_macos_tests.cpp)，CTest `osal.process_exit_macos`，
編譯真正 Mac monitor，只在測試編譯時替換 `kevent64` 呼叫，檢查註冊的完整代號與旗標、
32 個事件批次、非退出事件過濾、50ms／無期限兩種等待，以及 EINTR、syscall error、
混合 NOTE_EXIT／EV_ERROR 的原生診斷。沒有正式環境變數或 fault hook。
此案例不宣稱重新完成真實 kernel 的 PID 重用驗證。

### Actual results / 實際結果

| 檢查 | Debug | Release |
| --- | --- | --- |
| 獨立 Common／OSAL 及測試建置 | 通過 | 通過 |
| 新增契約：14 個共用案例＋1 個 Mac backend 案例 | 15/15 | 15/15 |
| 新增契約每項連續重跑 10 次 | 150/150 執行通過 | 150/150 執行通過 |
| 相關回歸整組：上述 15 項＋process／output／faults＋8 項 FD＋descriptor | **27/27** | **27/27** |

初次相關回歸在受限執行環境中各為 24/27，`fd_spawn.producers`、`fd_spawn.contention`、
`descriptor` 因共享記憶體／socket 的 native EPERM 失敗。保留原始紀錄；在允許本機操作的
環境整組重跑，兩組才得到 27/27。沒有改測試斷言或跳過失敗項目。
建置仍有原有 Pipe initializer 與舊 chdir_np API 棄用警告，沒有藉此擴大修改範圍。

五個外部 Debug 退化副本均被測試拒絕，沒有修改正式來源來開關 fault：

| 刻意移除的保護 | 拒絕該退化的案例 |
| --- | --- |
| pending 啟用補查 | `one-shot`：背景回收未完成 |
| starting 保存事件 | `early-publication`：背景回收未完成 |
| 事件代號精確匹配 | `stale-forgotten`：新紀錄錯誤進入輪詢 |
| 完成／失去 owner 清除 pending | `record-state`：殘留 pending 被偵測 |
| 原生 `EV_UDATA_SPECIFIC` | `process_exit_macos`：watch 身分旗標檢查失敗 |

一個初次退化產生器錯誤替換了額外的 if 語法，未成功編譯，另存為 invalid mutation；
修正產生器後才計入上述五個結果。編譯失敗不當成契約測試成功拒絕產品退化。

### Reproduction and remaining work / 重現與剩餘驗證

```sh
cmake -S src/vinc/osal -B /tmp/open-vinc-exit-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build /tmp/open-vinc-exit-debug --parallel 4
ctest --test-dir /tmp/open-vinc-exit-debug -R '^osal\.process_exit' --output-on-failure
ctest --test-dir /tmp/open-vinc-exit-debug -R '^osal\.(process|process_output|process_faults|process_exit_progress\..*|process_exit_macos|fd_spawn\..*|descriptor)$' --output-on-failure
```

Release 使用獨立目錄；Windows／Linux 不會登記 Mac 專用案例。
本步外部工作目錄：`/private/tmp/open-vinc-exit-step2-20261103`；持久交付：
`/Users/jay/Downloads/open-vinc-exit-step2-20261103/evidence.tar.gz`，附逐檔與壓縮檔 SHA-256。
完整框架、App／Plugin／Python／乾淨 SDK 與大量原生退出對照屬第三步，第二步未執行；
後續結果見 [第三步報告](MACOS_EXIT_VALIDATION.md)。Windows／Linux、舊 macOS、Intel Mac
與其他工具鏈尚未補驗。沒有 benchmark。

## Step 1 archived reproduction / 第一步歷史重現與交付

以下指令僅適用第一步結果包內的來源；目前工作樹已用正式測試取代舊 target。
從第一步來源 root，以可用的 C++23 工具鏈分別設定 Debug／Release，例如：

```sh
cmake -S src/vinc/osal -B /tmp/open-vinc-exit-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build /tmp/open-vinc-exit-debug --target osal_process_exit_progress_probe
/tmp/open-vinc-exit-debug/tests/osal_process_exit_progress_probe one-shot
/tmp/open-vinc-exit-debug/tests/osal_process_exit_progress_probe retry-control
/tmp/open-vinc-exit-debug/tests/osal_process_exit_progress_probe immediate-control
/tmp/open-vinc-exit-debug/tests/osal_process_exit_progress_probe periodic-control
```

第一步來源的第一個模式應返回 1，其餘返回 0；用法或 fixture failure 返回 2。
Release 使用不同 build directory 與 `-DCMAKE_BUILD_TYPE=Release`。
這些是手動診斷，不加入既有完整 CTest 的通過數字。

本輪外部工作目錄：`/private/tmp/open-vinc-exit-step1-20261103`。
持久結果包：`/Users/jay/Downloads/open-vinc-exit-step1-20261103/evidence.tar.gz`。
包含實際命令、工具、編譯／執行紀錄、每次退出碼、原生代號探測及本步來源／文件，
另附逐檔與壓縮檔 SHA-256；不包含建置 binaries。沒有重跑完整框架、SDK、Windows／Linux
或 benchmark，不沿用上一輪全套通過數字當成新設計的驗收。

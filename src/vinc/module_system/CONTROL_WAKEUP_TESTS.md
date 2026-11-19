# Control wakeup controlled tests / 控制喚醒受控驗證

These are dated validation records, not current implementation steps.
歷史結果與原始失敗保留當時範圍；文中的「待驗」不代表目前仍未完成。
Current contract / 現行契約：[contract](CONTROL_WAKEUP.md)。
Integration and corrected evidence: [acceptance](CONTROL_WAKEUP_VALIDATION.md). Latest native macOS coverage / 後續 Mac 範圍見 [2026-11-19 report](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)。

## Stage one evidence / 第一階段受控證據（2026-11-16）

**Confirmed:** the central inbox and supervisor wait are disconnected. This is
not evidence that Socket delivery, the child's reply receiver or Bus registration
processing itself takes a polling interval. Production wakeup was still unfixed at stage one;
stage three below replaces this behavior.

**已確認：** 應用 frame 已經進入中央收件匣，中央仍會睡到監督補查期限。
Windows／WSL 各三次獨立執行，每次五個受控交接，**各 15/15** 觀察到此路徑。
不是用「延遲超過某個毫秒值」當作根因斷言，而是核對：

1. 真正的 supervisor 完成一輪掃描，停在有期限的測試 gate；不持有 registry／record／peer mutex。
2. 真正的 Host／Plugin 回覆命令；reactor 驗證、入列後才允許測試釋放 gate。
3. supervisor 接著執行原本的 `wake.wait_for(20ms)`，實際回傳 `cv_status::timeout`。
4. 之後下一輪 `try_receive()` 才取出同一個 request ID 的 frame。

順序由 condition variable／受控檢查點確認，不使用固定 sleep 假設對端已到位。
另外在 supervisor 保持 gated 時直接使用同一 Session 的 `receive_until()`，
三次均可收到 matching reply，作為接收 API 的正向對照；它不保證每次呼叫都先進入阻塞。
所有 gate 最多 8 秒；失敗 unwind 先解除 gate，再由既有 Scope 停止／回收自建程序。
正常採樣確認 Host 真正以 exit code 0 結束，才讀取子程序 trace。

### Workload and measurements / 負載與分段量測

沿用 `messageBusTest` Plugin，先完成原有初始 Subscriber，再循序執行 64 次
`hold-callback` 命令；每筆仍各自進行 Subscribe 與 EnableNotifications。
不改 callback 容量、Module startup policy、輪詢間隔或 Windows timer resolution。
這是 Ready **之後**的訂閱負載，並含命令交付成本，不能當成外部 491 筆 startup 的通過證據。

每平台三次、每種操作每次 64 筆。下表列出三次各自 P50 的範圍（ms），
完整 P50／P95／P99／min／max、五次 gate 結果、退出碼及原始碼 SHA-256 保存在
[機器可讀基線](tests/baselines/control_wakeup_stage1.json)。P99 只有每次 64 筆，不能視為穩定尾延遲估計。

| 環境／操作 | 中央入列→取件 | 中央取件→回覆入列 | 子程序送出入列→回覆取件 | 子程序回覆入列→取件 |
| --- | ---: | ---: | ---: | ---: |
| Windows Subscribe | 29.944–30.017 | 1.121–1.134 | 31.556–31.853 | 0.031–0.034 |
| Windows EnableNotifications | 29.192–29.403 | 0.193–0.197 | 29.834–30.050 | 0.036–0.039 |
| WSL Subscribe | 19.602–19.615 | 1.042–1.052 | 20.945–20.965 | 0.039–0.042 |
| WSL EnableNotifications | 18.716–18.744 | 0.096–0.099 | 19.137–19.160 | 0.042–0.048 |

中央取件到回覆入列包含 Bus worker 排隊、處理與回覆 enqueue，不冒稱純計算時間。
子程序送出到取件不包含其後 `Context::subscribe()` 呼叫端恢復的全部成本。
只在同一程序內相減 steady-clock timestamps；兩程序使用 request identity 配對。
其餘來回傳輸／子程序派送成本以 duration 差值統計，不聲稱單向 wire latency。
所有 hook 使用固定容量 trace，熱路徑沒有寫檔或新增逐 frame 配置；trace mutex／時間戳
仍有量測成本，尚未量化，不能將這些 Debug 數字當成未插樁 Release benchmark。

Windows 五次受控等待各輪約 29.3–31.6 ms，WSL 約 20.09–20.20 ms；
20 ms 是等待政策，不是排程硬上限。本輪未單獨追查 Windows timer 粒度，
也沒有用縮短 timer 作修復。64 筆完整命令負載為 Windows 5.969–5.991 s、
WSL 3.876–3.878 s。主要改善方向仍是可靠接收喚醒，不是合併 API 或放寬 startup timeout。

### Reproduction and boundaries / 重現方式與邊界

以下為第一階段當時的重現方式；第三階段已將 CTest 改名為 `module.control_wakeup`，
且預設要求修復成功。歷史 JSON／失敗紀錄不改寫成新版本結果。

建置 target `module_control_wakeup_tests` 會一起建立測試 Host 與 Bus Plugin。
正式 Manager／Control Channel 不定義 `VINC_CONTROL_WAKEUP_TESTING`，其行為不變；
只有隔離測試重編實際來源並啟用觀察 gate。一般 CTest：

```text
cmake --build <build> --target module_control_wakeup_tests
ctest --test-dir <build> -R "^module\.control_wakeup_baseline$" --output-on-failure
```

這是**目前缺口的 characterization**，通過代表重現現況，絕不代表修復完成。
保留應失敗的修復契約模式，不用 `WILL_FAIL` 把故障偽裝成修復通過：

```text
<test-executable> <test-host> <message-bus-test-plugin> <absolute-output-directory> --require-wakeup
python src/vinc/module_system/tests/control_wakeup_analyze.py <absolute-output-directory>
```

Windows 的檔案位於 `<build>/framework/module_system/tests/` 與
`<build>/framework/message_bus/tests/message_bus_module_helper.dll`；WSL Plugin 為
`libmessage_bus_module_helper.so`。未給 output directory 時使用每次獨立的暫存目錄，
成功清理後不留下 trace。提供目錄時保留 `parent.csv`、`child.csv`、`evidence.txt`；
每次採樣應使用不同目錄。分析器拒絕重複 identity、缺失配對及不符預期的筆數。

兩平台第三次採樣以 `--require-wakeup` 執行，均在**正常收尾完成後以 1 退出**，
明確指出 5/5 queued replies 睡到 fallback；fixture／其他斷言失敗用退出碼 2。
第三階段需將正式 CTest 切成修復契約，移除「必須重現缺口」的 characterization 斷言，
再擴充掃描中、等待中、backlog、公平性及 RPC deferred 案例；本輪不宣稱已覆蓋那些競爭。

實際環境：同一台 AMD Ryzen 7 5700X3D（8C/16T），Windows 11 Pro 26200、
MSVC 19.51.36257／toolset 14.51.36231、CMake 4.3.2；WSL2 Ubuntu 24.04.5、
kernel 6.18.58.1、GCC/libstdc++ 13.3.0、glibc 2.39、CMake 3.28.3。皆 Debug。
六組正式採樣循序執行，沒有同時跑另一平台的基線或 build；未隔離整台開發機的其他負載。
來源為上方 HEAD 加上既有未提交 Bus 修復與本輪測試，不能只 checkout HEAD 重現。

兩平台既有 Debug build 重建受影響 target 後，相關 CTest **Windows 7/7、WSL 7/7** 通過：
`control.channel`、`control.faults`、`message_bus.module`、`module.control_wakeup_baseline`、
`module.context`、`module.process`、`module.manager_faults`。其中 baseline 的綠燈只代表
現況重現成功；上述單獨執行的 `--require-wakeup` 仍是紅燈。這不是全專案或乾淨 SDK 驗收。
日誌為下述平台目錄中的 `build.log`／`ctest.log`。

本機原始輸出：`build/control-wakeup-stage1/{windows-debug,wsl-debug}/baseline-{1,2,3}/`。
初輪 fixture 曾將登記 ModuleType 寫成不同於 Plugin 宣告的名稱，正確收到 InvalidFactory；
已修正測試宣告，未修改 Runtime 的型別檢查。初輪結果不列入以上採樣。
Release、macOS、SDK、效能成本與外部應用仍依後續階段驗收，不以本輪 Debug 結果代替。

## Stage two validation / 第二階段通知原語與生命週期（2026-11-16）

ReceiveWake is implemented in the existing Control Channel target; the central
supervisor does not bind it yet. A coalesced pending bit, condition variable and
the same mutex cover signal, consume and wait. Session publication/binding uses
Peer → ReceiveWake ordering with a weak association and temporary strong signal
owner. This supplies stage three's building block; it does not fix the measured
central latency on its own.

原語已實作，中央尚未使用。新增內部 detail header 與指定 friend 接線，不增加
Session 公開方法、wire 欄位、worker 或命名 OS 通知。新增 ControlChannel
`Cancelled=12`、`WaitReceiveWake=17` 及靜態文字表；既有錯誤編號不變。
API、消耗順序、等待優先序與生命週期的主要維護位置見上方 Control Channel README。

| 驗證 | Windows | WSL Ubuntu 24.04 |
| --- | --- | --- |
| Debug 相關回歸 | 8/8 | 8/8 |
| 最終 Debug `control.receive_wake` 連續執行 | 50/50 | 50/50 |
| Release `control.channel`／`control.faults`／`control.receive_wake` | 3/3 | 3/3 |

Debug 矩陣為第一階段的七項加上 `control.receive_wake`。完整矩陣通過後，
再補入「通知途中重綁」與「兩個 Session 共用 owner」測試，重建並於各平台
連跑最終版本 50 次；這次補充沒有改正式實作。Release 三項使用最終測試版本。
`control.channel` 直接連結正式 library 驗證通知及原 receive API；`control.receive_wake`
則隔離重編相同來源，以測試專用 gate 固定交界。`control.faults` 延用既有注入，
新增檢查 Poller failure／worker exception 會通知既有 Session 及快照後才 attach 的 Session。

受控案例包括：入睡前已有提示、consume／掃描期間提示、持鎖到原子等待的交界、
虛假喚醒後重新檢查 predicate、停止喚醒、取消不清除 pending、等待者持有 owner、
綁定前 backlog、Open／握手／Closed、正常 heartbeat 不通知、32 幀合併與原 FIFO、
解除後不再通知舊 owner、通知途中釋放 owner、通知途中重綁、owner 過期後仍可收件、
Session 比 Service 長壽、兩個 Session 共用通知而互不傳播關閉。
gate 有 5 秒期限，釋放 gate 先於等待測試 thread 結束；CTest 另有限時。
這些測試固定關鍵順序，不是全排程競爭或記憶體 sanitizer 的證明。

建置／重現方式：

```text
cmake --build <build> --target control_channel_tests control_channel_fault_tests control_receive_wake_tests
ctest --test-dir <build> -R "^control\." --output-on-failure
ctest --test-dir <build> -R "^control\.receive_wake$" --repeat until-fail:50 --output-on-failure
```

工具鏈／硬體與第一階段相同，既有 Debug／Release build 重建；原始日誌保存於
`build/control-wakeup-stage2/{windows-debug,windows-release,wsl-debug,wsl-release}/`，
包含 `build.log`、`ctest.log`，Debug 另有 `repeat-build.log`／`repeat-ctest.log`。
Windows 本輪建置無編譯警告；WSL 仍出現既有部分 aggregate initializer 與
隔離 Manager 測試的 internal-linkage 警告，未擴大修改與本輪無關的程式。

第一階段 characterization 在兩平台 Debug 仍通過，表示中央缺口仍可重現。
中央管理／停止提示接線、16 幀公平排程、RPC deferred 不忙迴圈、持續流量下的
健康補查期限，以及完整 SDK／成本驗收尚未完成。macOS 原生驗收仍在 TODO。
第一階段 JSON 的 source hashes 描述當時基線，不應拿來聲稱現在來源仍完全相同。

## Stage three: central scheduling / 第三階段中央排程（2026-11-17）

The supervisor owns one shared ReceiveWake and binds each newly attached Session.
Management/start/stop use the same pending bit. It consumes before copying/scanning
records and waits without registry/record locks. The owner survives supervisor join
and channel teardown; Sessions retain weak associations. No worker, wire field,
public Context API or named OS notification is added.

中央持有一份 ReceiveWake；新 Session 綁定時補查既有狀態／backlog。管理與停止
使用同一 pending 交接，consume 在掃描之前，不在掃描後清除新通知。既有析構先停
並 join supervisor，再釋放 Session／服務；通知 owner 最後才釋放。

`receive_control_frames()` reports whether its 16-frame budget was exhausted with
potentially runnable input. Every other Record finishes that pass before the next
immediate scan. No seventeenth frame is consumed to peek at readiness. An exactly
full batch with no remaining input can cause one harmless extra empty scan.
Fatal → control → OS-exit ordering and first-failure/cleanup rules remain unchanged.

每 Module 每輪仍最多取出 16 幀；用完額度的可處理 backlog 接續下一輪，其他 Module
不必等待該 Session 清空。保留 FIFO，不為探查是否還有資料偷取第 17 幀。
RPC deferred 是已取出的原幀，重試不重新取件，也不重送已成功接受的請求。

The 20 ms health deadline advances only when due, never on each incoming hint.
Lifecycle/exit/stop checks still run on each scan. Deferred RPC admission is retried
only on a due health pass; QueueFull keeps that original frame and blocks later
frames from the same Session. Incoming hints cannot promote it to runnable backlog.
Once admitted, the retained frame is cleared and FIFO resumes. The first retry can
use the next already scheduled health tick: this is a shared fallback cadence,
not a fresh per-request delay or a hard real-time guarantee.

20 ms 是共用的絕對補查節奏，流量不重設它；RPC 滿載只於到期輪次重試，不用 backlog
自行喚醒。第一次重試可能接上已排定的下一次補查，不承諾每筆都先等滿 20 ms。
停止／退出監督不依賴 RPC 騰出容量；退出確認才允許清理跨程序資源。

### Controlled coverage / 受控驗證

- `module.control_wakeup` now requires all five queued-before-wait handoffs to
  return without a fallback timeout. It retains the direct receive positive
  control, 64 real callback registrations, normal Host exit and causal trace.
  The test synchronizes with the end of Peer publication before releasing its
  gate, so observing the pre-notify Queued hook cannot race the assertion.
- `module.control_scheduler` runs two real OSAL child/control peers. One queues
  40 frames and the other one frame before releasing the supervisor gate. It
  asserts FIFO, 16/16/8 processing passes, second-peer progress in the first pass,
  and no wait between runnable batches.
- Existing RpcManager test friendship parks its real worker while idle and fills
  the actual bounded input queue. Real `submit()` returns QueueFull; no fake error
  or replacement admission implementation is used. Three original frames and a
  trailing FIFO marker exercise retained ownership. Other-peer traffic is followed
  by five additional quiet health retries. Pass/wait counts and due deadlines
  verify no self-wake/busy loop or traffic-postponed fallback.
- Releasing capacity admits the original three IDs exactly once and in order;
  the trailing marker remains blocked until then. A second full-queue cycle
  verifies the blocked Module itself can exit before downstream capacity returns.
  Another Module stops during the first saturation cycle. A pre-wait shutdown
  gate verifies management handoff. Both child exit codes and the absence of
  primary failures are checked before cleanup.

受控測試使用有期限的 condition-variable gates，沒有用固定 sleep 代替因果同步。
RPC worker 只在測試中暫停，佔位資料不執行；此案例證明中央排程、真正的滿載判斷、
入列順序與回收，不把空 payload 的測試幀說成有效 RPC handler 執行。實際 RPC
請求／回覆、同步、異常退出及 handler failure 由下面既有回歸測試補驗。
這不構成跨崩潰 exactly-once、一般 deadlock freedom 或 CPU 成本的保證。

### Validation / 驗證結果

| Scope | Windows | WSL Ubuntu 24.04 |
| --- | --- | --- |
| Debug related regression | 25/25 | 25/25 |
| Debug wakeup and scheduler, 20 consecutive runs each | 40/40 | 40/40 |
| Release control + scheduler + process RPC regression | 10/10 | 10/10 |
| Final test-initializer cleanup: Debug + Release, five runs of each new test | 20/20 | 20/20 |

Debug matrix: `control.channel/faults/receive_wake`, `message_bus.module`,
`module.control_wakeup/control_scheduler/queries/context/shutdown_evidence/process/system/manager_faults/runtime_faults/plugin/failure_order/debug`,
and all nine C++ `rpc.*` tests. Release covers the three Control tests, the two new
Module tests and `rpc.process/target_exit/handler_failure/synchronous/synchronous_target_exit`.
These are related source-tree tests, not full-project, Python or clean installed-SDK acceptance.

Windows：MSVC 19.51.36257／toolset 14.51.36231、Windows 11 Pro 26200、CMake 4.3.2。
WSL：Ubuntu 24.04.5、GCC/libstdc++ 13.3.0、glibc 2.39、CMake 3.28.3。
同一台 Ryzen 7 5700X3D；既有 Debug／Release build 重建。部分平台／組態的建置與
驗證並行，沒有隔離整台機器負載，因此本輪不提供正式效能對照或 CPU 改善數字。
測試內容為 `fc27d2788e74a163ea597f4e736004dbcfc6d529` 加既有 Bus 修復與本輪未提交修改，
不是僅該 commit。Windows 無新增編譯警告；WSL 隔離 Manager 重編仍出現原有
aggregate initializer／internal-linkage 警告，不擴大修改無關實作。
Release 另在既有 `record_failure()` 發出 `-Wdangling-pointer`；值複製處未確認存在
實際懸空引用，已在 [README 待辦](README.md#outstanding-issues--待處理事項) 記錄最小重現後續。

初輪新 scripted Host 漏 include 錯誤協定標頭而無法編譯，補齊後完成上述驗證；
未更改正式錯誤常數。最後另明確初始化新測試 Frame 的空 payload，消除新測試自身
的 initializer 警告，重新建置並驗證；既有未初始化欄位的值原本也是空 vector。

Reproduction:

```text
cmake --build <build> --target module_control_wakeup_tests module_control_scheduler_tests
ctest --test-dir <build> -R "^module\.control_(wakeup|scheduler)$" --output-on-failure
ctest --test-dir <build> -R "^module\.control_(wakeup|scheduler)$" --repeat until-fail:20 --output-on-failure
```

本機日誌：`build/control-wakeup-stage3/{windows-debug,wsl-debug,windows-release,wsl-release}/`。
Debug 擴大矩陣為 `final-build.log`／`final-ctest.log`，連跑為 `final-repeat.log`；
Release 為 `build.log`／`ctest.log`。最後空 payload 初始化的收尾重建／測試為各組態
`closure-build.log`／`closure-ctest.log`。第一階段 JSON 的 source hashes／基線保持原樣。

第三階段當時尚缺整合、SDK 與成本；後續已完成 Windows／WSL [第四階段驗收](CONTROL_WAKEUP_VALIDATION.md)，保留首輪失敗與重跑限制。
macOS 原生測試與外部應用 491 筆／30 秒 Ready 驗收保留於根 TODO，不宣稱本輪已完成。

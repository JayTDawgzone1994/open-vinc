# Central control wakeup / 中央控制接收喚醒契約

**Status: stages one through four completed on Windows/WSL (2026-11-17). The [macOS follow-up](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md) records later native results.**
The central supervisor wakes on received control work while retaining its
single-owner lifecycle model, bounded fair passes and 20 ms supervision fallback.
This document owns the cross-component change contract and acceptance plan.
It does not authorize later stages or imply completion beyond the recorded validation.

**狀態：2026-11-17 Windows／WSL 四階段完成；Mac 原生結果見上述報告。** 最新整合、乾淨 SDK／搬移與成本結果見[第四階段驗收](CONTROL_WAKEUP_VALIDATION.md)。各階段仍依
[AGENTS](../../../AGENTS.md) 的開工規則執行。Control Channel 的一般契約見
[README](../control_channel/README.md)，Module 生命週期見 [Module System README](README.md)。

## Problem and evidence / 問題與證據

Control Channel 的 `incoming()` 已在應用 frame 入列後通知 `Peer::changed`，
`Session::receive_until()` 會等待它；中央 `ModuleManager::monitor()` 卻用
`try_receive()` 掃描各 Record，之後在另一個 `wake` condition variable 等待約 20 ms。
修復前沒有將接收就緒接到中央等待點；第三階段已補齊。這與已實作的
[資料通知及 pump 喚醒](PUMP_WAKEUP.md)、[Bus 拓樸補同步](../message_bus/CONTROL_PLANE_REPAIR.md)
是不同路徑；先前功能驗收不能作為本次啟動延遲的效能證據。

來源 review 基線：本儲存庫 `fc27d2788e74a163ea597f4e736004dbcfc6d529`
加上尚未提交的 Bus 控制面修復；外部應用使用另一份乾淨 pin
`04fe4ee69e1d8c83fe39dcb27afde879f053b365`。兩者 `channel.cpp` 相同，
中央等待路徑也存在同一缺口，但 callback 容量／bootstrap 接線不同，不能混用產物。

外部應用提供的一次 Windows 同 binary／腳本對照：491 筆 callback 訂閱各做
Subscribe 與 EnableNotifications 兩次同步請求。基線撞到 30 秒 Ready 期限；
Subscribe 中位數 31.448 ms，其中送出入列到中央取件約 30.877 ms。
僅對中央套用 1 ms timer request 後，fixture 啟動 23.759 s，Subscribe 中位數
21.042 ms，其中約 20.506 ms 在中央取件前。這支持中央等待主導的判斷，
但時間戳不是實體 wire 時間，也不是本儲存庫已獨立重現或三平台因果證明。
原始對照 `central-timer-r1/comparison.json` SHA-256：
`2898f9251c0399b005eac44e67e7b9a43a3dce9aa14dc18c294aaecd78a4f172`。
第一階段須建立框架自己的受控證據；不採用 timer request 作為正式修法。

## Confirmed scope / 已確認範圍

- 中央使用共用接收喚醒，維持單一 supervisor；不增加每 Session 接收執行緒。
- 保留 20 ms 健康／退出／延後工作補查。這是檢查政策，不是硬即時上限。
- 每 Module 每輪維持最多 16 幀，保留各 Session FIFO 及跨 Module 公平處理。
- 一起處理通知遺失競爭、綁定前 backlog、滿載、停止、生命週期與成本驗證。
- Subscribe 與 EnableNotifications 本輪仍為兩次請求；不合併註冊協定。
- 公開 Context、pump、訊息布局、wire 協定、既有期限與故障／回收政策不變。
- 不全面重寫 Socket／程序／計時器統一排程器，不加入跨程序命名通知資源。
- 不修改外部應用 checkout、容量、訂閱數／順序、Ready 期限、pin 或部署。
  外部應用整合另由其維護者授權與驗收。

## Ownership and notification / 責任與通知

Control Channel 提供指定的 `detail` 接線，僅表示「有狀態值得接收者查看」，
不解讀 Module、Bus、Param 或 RPC 的業務語意。第二階段的指定內部介面為
`control::detail::ReceiveWake`／`ReceiveWakeAccess`；具體 API 與鎖契約由
[Control Channel README](../control_channel/README.md#internal-receive-wake--內部接收通知原語) 維護，
不是 Module 公開 API。

中央持有程序內喚醒狀態；Session 使用弱引用或具等效安全性的關聯。
訊息只保存在原接收佇列，不建立第二份訊息或無界事件清單。
沒有綁定的 Session 繼續使用原本 receive API。

通知涵蓋已驗證的應用訊息成功入列、握手完成、Session 終止，以及綁定當下
已存在的待收資料／相關狀態。正常 heartbeat 不逐筆喚醒中央；失聯導致的終止需通知。
通知不代表業務操作成功、程序已退出或共享資源已可回收。

I/O thread 只更新通知狀態，不執行 ModuleManager、BusManager 或使用者 callback。
通知本身不新增逐 frame 配置、worker、Socket 或命名 OS handle。
初始資源取得沿用 Result 與元件錯誤包裝；既有配置例外邊界保持，
不得把已入列的工作因通知問題偽裝成「尚未送出」而重播。

## Waiting and fairness / 等待與公平性

以可合併的 pending 狀態記住提示，狀態更新與等待 predicate 使用同一把專用 mutex。
每輪開始前消耗既有提示，再檢查實際佇列；掃描期間的新提示保留到下一輪。
掃描到入睡之間收到提示時不得睡到 fallback；不只加裸 `notify_all()`。
綁定、解除綁定與既有 backlog 的交接必須受同步保護，不依賴下一筆新訊息。

| 處理結果 | 後續行為 |
| --- | --- |
| 本輪已無可處理工作 | 等接收／管理／停止提示或監督期限 |
| 每 Record 額度用完，可能仍有可處理工作 | 先讓其他 Record 完成本輪，再安排下一輪；不得遺忘 backlog |
| 下游暫時不能接收，例如既有 RPC deferred request | 保存原請求及順序，依補查時機再試；不得因佇列非空持續自喚醒 |
| Session 或程序故障 | 進入既有故障及確認退出流程，通知不代替退出證據 |

第一版維持既有有界 Record 全體掃描，不另建一份永久的 Module ready queue。
管理操作與停止要求也使用可靠的提示交接，不能只修網路入列而留下另一個 lost wake。
應用訊息不得反覆重設健康補查的絕對期限；繁忙時仍須推進停止、故障與期限檢查。
不為追求吞吐跳過其他 Record 或改變 fatal、控制訊息、退出觀察的既有優先序。

RPC deferred 工作的重試節奏要與立即可處理的收件工作區分；本輪沿用 20 ms 補查，
不擴建所有下游元件的可寫通知，也不改既有滿載錯誤契約。
持續流量可能增加全體掃描成本，必須實測，不能由 Record 上限推論成本可忽略。

## Locks, shutdown and compatibility / 鎖、停止與相容性

- 等待 mutex 不包住 Record 掃描、frame 派送、使用者 hook 或 thread join。
  I/O 通知不得反向取得 Manager registry／record mutex。
- 保留 Control Channel 的 Engine → Peer 鎖順序；新增通知鎖順序須明列並測試。
- Session 綁定／解除與晚到通知同步；舊世代提示最多造成額外檢查，不能操作新 Record。
- 停止會喚醒等待者；等待者及已取得的通知強引用結束前，狀態不能銷毀。
  不持有 registry／record／wake mutex 等待 worker join。
- 維持第一個主要故障、獨立 cleanup 診斷、正常完成與重啟政策。
  只有確認 OS 退出才能回收跨程序共享資源。
- 預期不改 wire 或 Plugin 公開布局；實作仍須審核 Session/Pimpl、detail headers、
  static targets 及 SDK 匯出。不得單憑版本號未變承諾二進位相容。
  交付需記錄實際重建要求，不混用兩份應用 pin 的 Host／Plugin／binding。

## Stages and acceptance / 開工步驟與驗收

| 階段 | 工作與完成條件 |
| --- | --- |
| 1．受控根因與基線 | 建立框架內隔離測試，明確觀察 frame 已入列但 supervisor 未立即取件；分開記錄入列→取件、下游處理及回覆時間。保留基線失敗，測試有期限且清理自建程序。不能只用總啟動耗時或固定 sleep 宣稱因果。 |
| 2．通知原語與生命週期 | 完成 Control Channel 指定 detail 接線及單元測試。以 barrier／測試同步點驗證綁定前、掃描中、入睡交界通知，包含合併、虛假喚醒、解除／重綁、過期 owner、Session 關閉與 worker 故障；既有 receive_until 使用者不受影響。 |
| 3．中央接線與公平排程 | 接收、管理操作、停止使用同一可靠等待契約；保留 16 幀額度及 20 ms 補查。多 Session／超過額度仍保持 FIFO、其他 Module 進度與停止期限；注入 RPC 下游滿載，驗證不重播、不忙迴圈、不餓死監督。 |
| 4．整合與成本 | Windows／WSL Debug、Release 跑 Control Channel、Module 生命週期／故障／退出、Bus、RPC、Param 與乾淨 installed SDK／搬移回歸。比較少量與大量連續控制要求的延遲、啟動時間、CPU、閒置喚醒、執行緒／handle／記憶體。Mac 原生另列交付驗收。 |

量測須使用相同負載、容量、順序、期限與工具鏈；記錄 commit／工作樹、平台、
取樣次數及其他負載。比較延遲分布與 CPU，不只挑一次最快結果。
不能以縮短輪詢、放寬 Ready、提前 Ready、減少訂閱或平行改寫測試掩蓋問題。
測試不得假設事件喚醒提供 OS 排程的硬即時保證。

外部應用最終驗收仍需原 491 筆、原順序、30 秒 Ready 期限，以及該專案原本的
完整測試／部署條件。框架可用通用負載先驗證，但不得把它宣稱為外部應用整體通過。
其 callback 容量設定與本儲存庫的差異需在整合時核對，不隨此修復偷偷改值。

## Progress / 目前進度

- [x] 設計範圍經使用者確認，契約文件化。
- [x] 第一階段：Windows／WSL Debug 受控根因與修復前基線；見下方證據與明確失敗的回歸模式。
- [x] 第二階段：接收喚醒與生命週期；Windows／WSL Debug／Release 原語測試通過。
- [x] 第三階段：中央公平排程及下游滿載，Windows／WSL 受控驗證通過。
- [x] 第四階段：Windows／WSL 整合、乾淨 SDK／搬移與 24 組成本對照；見[驗收報告](CONTROL_WAKEUP_VALIDATION.md)。
- [ ] macOS 同版原生驗收；外部應用同版整合另由其維護者完成。

若發現必須變更公開 API、wire、錯誤優先序或共享資源回收責任，先停止受影響部分
提出決策。新實作與驗證結果更新於本文件；既有 Bus 修復證據保留原範圍。

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

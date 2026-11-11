# Host shutdown repair contract / Host 收尾修復開工契約

Date / 日期: 2026-11-10.

## Status and ownership / 狀態與文件責任

**Stages 1–2 completed; stage 3 Windows/WSL acceptance completed.** The owner authorized stage 3
on 2026-11-10; subsequent native Mac C++ Host/Context Debug/Release acceptance
is complete in the [Mac report](MACOS_CONTEXT_SHUTDOWN_VALIDATION.md).
Production teardown ordering, Bus classification and first-generation
test diagnostics are implemented. Windows Debug / WSL Release focused evidence is
recorded in the [investigation](CONTEXT_SHUTDOWN_INVESTIGATION.md#stage-2-repair-validation).
The [regression report](CONTEXT_SHUTDOWN_VALIDATION.md) records broader results and
the WSL environment-only follow-up. The [Mac handoff](MACOS_CONTEXT_SHUTDOWN_HANDOFF.md)
lists the remaining native acceptance gates.

**第一、二階段完成；第三階段 Windows／WSL 驗收完成。** 本輪已執行配置內完整回歸；
後續 Mac C++ Host／Context 的 Debug／Release 原生驗收已完成，詳見上述報告；
來源 Python／GIL 及 SDK-only 交付仍依後續階段驗證。

This document owns the repair scope, target contracts and acceptance gates.
[Investigation](CONTEXT_SHUTDOWN_INVESTIGATION.md) owns the original observations,
probe counts and evidence limitations. [Module System README](README.md) owns
current lifecycle behavior; [Message Bus](../message_bus/README.md) and
[Control Channel](../control_channel/README.md) retain their component contracts.
Update implementation status only after recording actual validation.

## Evidence boundary / 證據邊界

- Confirmed in the pre-fix code: RuntimeGuard shuts down Bus before joining the watchdog. An iteration
  past the finished check can still dispatch into that closed transport.
- Confirmed: unchanged Host runs reproduced abnormal exits. Instrumented runs
  captured receive-after-close, NotificationRoutesChanged, BusFailed/Protocol and
  exit 72. Restart followed by an obsolete MID query masks the original failure.
- Not established for the initial intermittent probes: the first close source in every captured process, whether each
  main result was successful, or whether the rejected frame body was valid.
  finished/cleanup phase alone does not establish those facts.
- Stage 1 now establishes successful hooks/completion, first close by RuntimeGuard
  and a valid frame body in a controlled scenario on Windows and WSL. It does not
  retrospectively establish those facts for every earlier intermittent failure.
- The original stage-4 failure lacks sufficient diagnostics for unique retrospective
  attribution. Stress counts are not production failure-rate estimates.

已確認收尾順序缺口與關閉後派送；第一階段已在受控案例補齊首次關閉來源、收尾前結果
與有效封包證據。原始偶發紀錄的限制保留，不能以受控案例替它們補出不存在的紀錄。

## Repair contract / 修復契約

1. **Receiver lifetime precedes resource retirement.** Complete callback drain,
   on_stop, Module/callback destruction and required completion/failure exchanges
   under the existing lifecycle policy. At final teardown, publish finished and
   request watchdog stop, join the watchdog, then shut down Bus and close the
   lifecycle Session. Retain adapters and Plugin code for their existing required
   lifetimes. Never join while holding a mutex needed by the watchdog.
2. **Preserve supervision.** Do not stop the watchdog before user cleanup and
   required exchanges finish. Keep central supervision, original stop deadlines,
   failure precedence and restart policy. Do not restart a deadline to accommodate
   join. Reuse the existing bounded receive poll (20 ms default); it is not a hard
   wall-time guarantee or a bound on arbitrary in-flight work.
3. **Preserve real failures.** Do not treat all errors during stopping as success,
   and do not rely on a second unsynchronized finished check. An already-closed
   Bus receive reports the existing Closed classification; malformed protocol
   remains Protocol. Keep appropriate layer wrapping and cause chains. Audit
   callers/tests for the changed internal classification.
4. **Keep first-generation test evidence.** Normal-completion tests must expose an
   unexpected exit without a successful retry obscuring it. Check the retained
   generation, primary/cleanup errors and actual exit before querying an obsolete
   MID. Dedicated restart tests keep their explicit restart budgets.
5. **Review adjacent adapters within scope.** Check stop, receive, progress and
   destruction ordering for notification, Param, RPC, locks, queries and Log.
   Cancellation is not destruction. Do not move every stop call indiscriminately;
   preserve wake/cancel requirements and report any new major lifecycle conflict
   before changing it.

最終順序：完成原有使用者收尾／協定 → finished＋request_stop → join watchdog →
關閉 Bus／Session → 依既有所有權銷毀服務、最後卸載 Plugin。
不新增背景執行緒、公開 API、ABI、共享記憶體布局或可調參數；不改 pump worker
數量、正式重啟政策，也不以加長 timeout 或忽略錯誤作為修復。

## Stage 1: causal evidence and controlled regression / 第一階段：因果證據與受控重現

Status: **Completed / 已完成** (2026-11-10). The stage-1 revision of
`module.shutdown_evidence` deliberately asserted the pre-fix defect; its passing
result was not repair acceptance. Stage 2 updates the maintained test to require
the corrected order.
Windows Debug and WSL Release each passed the three focused tests and ten repeated
controlled runs. Windows natural observations: 100 cases, no exit-72 reproduction.
See the investigation for final-run checks, injection boundary and raw logs.

- Add test-only first-close diagnostics distinguishing final teardown from send,
  request or protocol failures; record the pre-teardown hook/completion result.
  Preserve first-cause identity instead of overwriting it on repeated shutdown.
- Use the existing isolated fault-test approach for bounded sequencing gates.
  Admit a valid Bus notification past the watchdog finished check, begin final
  teardown, then release dispatch. Gate coordination must not hold production
  locks needed by either participant. Gates must time out and clean up children.
- Establish an old-order failure with an exact expected diagnostic, not any
  nonzero exit. Preserve evidence before applying the fix. Keep a reproducible
  old-order comparison outside production code when moving to stage 2.
- Keep probes/checkpoints out of production builds. No production behavior,
  deadline, restart policy or success assertion relaxation in this stage.

驗收：能受控捕捉關閉後派送、確認首次關閉來源與收尾前結果；失敗時有 MID、階段、
原因鏈、退出狀態與清理資訊。若證據顯示另有先發故障，先回報並修訂分析，不硬套結論。
紀錄 Windows Debug 重現；跨平台接點不得引入 Windows 專用同步假設。

## Stage 2: production repair and focused tests / 第二階段：正式修復與針對性測試

Status: **Completed / 已完成** (2026-11-10). Windows Debug and WSL Release each
passed nine focused tests and ten controlled repetitions. Restoring only the old
Guard order in an isolated Windows Host makes the new test reject a valid frame
received after close (exit 72); changing classification alone does not fix lifetime.
See the investigation for commands, exact evidence and adapter audit.

- Correct RuntimeGuard ordering and audit the adjacent adapters against the
  lifetime contract above. Keep genuine faults and central supervision effective.
- Separate Closed from Protocol in the Bus receive path using existing error
  identities; retain validation and layer-specific wrapping.
- Repair success-case generation checks and diagnostics in context_tests;
  keep explicit restart-policy coverage independent.
- Make the stage-1 controlled scenario pass with the corrected ordering and
  verify the old-order comparison still detects the defect. Assert no Bus
  retirement before receiver exit; do not force the new implementation to close
  Bus merely to release a test gate.
- Test genuine malformed input, central disconnection and stuck callbacks/hooks,
  including first-failure retention and actual process exit/reclamation.

驗收：受控舊版失敗／新版通過；正常完成 exit 0、無非預期重啟；真實故障仍照原契約
處理。先跑 Windows Debug 與 WSL 針對性測試，不以重跑偶發成功取代受控證據。

## Stage 3: regression and documentation / 第三階段：跨平台回歸與文件收尾

Status: **Windows/WSL acceptance completed / Windows／WSL 驗收完成**.
Actual configurations, results and preserved failures are in the
[regression report](CONTEXT_SHUTDOWN_VALIDATION.md). Native Mac execution is explicitly handed off
in [MACOS_CONTEXT_SHUTDOWN_HANDOFF.md](MACOS_CONTEXT_SHUTDOWN_HANDOFF.md).

- Run Windows Debug/Release and WSL Ubuntu 24.04 affected Module lifecycle,
  Context, parallel-pump/stop, Bus, fault and recovery tests. Include Manual and
  FrameworkLoop, serial and multiple Topic workers, and short-lived Modules
  with registration/unsubscribe and route changes during teardown.
- Repeat the formerly intermittent case with recorded counts and workloads.
  Confirm normal completion, no unintended restarts and no leftover processes
  or owned resources. Keep failed runs, not only successful reruns.
- Run the broader configured project regression after focused checks pass.
  Record build configuration, exact revision/working-tree state, toolchain,
  commands, results and log locations. Separate unrelated failures explicitly.
- Update README, investigation report and TODO from measured results; retain
  historical evidence and link this contract rather than duplicate it.
- **Native macOS Debug/Release remains mandatory pending acceptance.** Run the
  same controlled test, short-lived Module repetition, lifecycle/fault/stop and
  parallel-pump regression there. Windows/WSL success cannot stand in for macOS.

驗收：逐平台列出實際結果與缺口；Mac 未實測時維持 TODO 待驗，不宣稱三平台完成。
本契約不包含 commit、push 或合併；發佈依使用者另行指示與 AGENTS Git 流程。

## Expected change locations / 預計改動位置

| Area / 區域 | Scope / 範圍 |
| --- | --- |
| Module runtime / `src/runtime/module_runtime.cpp` | Final teardown order and necessary adapter wiring / 最終收尾與必要接線 |
| Module tests / `tests/` and test CMake | Isolated checkpoints, causal diagnostics, regression and generation assertions / 隔離接點、因果診斷、回歸與 MID 斷言 |
| Message Bus / `../message_bus/src/message_bus.cpp` and tests | Closed versus Protocol, test-only close provenance / 關閉分類與測試用來源紀錄 |
| Module/Bus README, investigation and root TODO | Evidence, completed stages and remaining platform acceptance / 證據、進度與平台待驗 |

If implementation needs a new responsibility boundary, public interface, deadline
policy or thread, stop that affected part and discuss before expanding scope.
若需要上述範圍以外的新架構取捨，先暫停受影響部分並提出討論。

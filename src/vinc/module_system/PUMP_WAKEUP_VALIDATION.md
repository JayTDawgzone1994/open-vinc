# Pump wakeup validation / pump 喚醒驗收

These are dated evidence records. Follow [the contract](PUMP_WAKEUP.md) for current
behavior and [the latest Mac report](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)
for the later integrated revision. Historical pending notes describe their original checkpoint.
以下保留原始量測與各輪驗收；較早的「待驗」不取代後續報告。

## Motivation and evidence / 問題與證據

Before this change, `Context::pumpLoop()` calls `pump_batch()` and waits when it dispatches no callbacks or commands. Host `wait()` uses `module_pump_idle_interval` (10 ms); its predicate covers stop, commands, and RPC readiness, not shared-memory Bus publication. A publication does not itself wake this wait.

修改前，沒有 callback 或命令時，`pumpLoop()` 使用預設 10 ms 等待；Bus 發布到共享記憶體不會直接喚醒它。

The 2026-10-28 investigation used source `91d26bca95d80a6354323c58bea3f5fab9f9fe2c`, Windows 10.0.26200 x64, Ryzen 7 5700X3D, MSVC 19.51, and Release. Each case used 1 MiB, eight slots, 64 warmups, and 1,000 measured publications at a requested 100 Hz. Across twelve cases, all 12,000 measured messages arrived without collisions, overwrites, or integrity errors. Default pump P99 was approximately 16 ms; continuous `pumpOnce()` reduced measured P99 below 0.7 ms but consumed approximately one logical core. Instrumented default waits had medians of 15.2–15.4 ms. These are environment-specific observations, not hard latency bounds or measurements of the proposed implementation.

調查重現約 16 ms 的尾延遲；持續 pump 可降低延遲，但約占一個邏輯核心。這支持改進等待策略，不代表已驗證新通知方案，也不是三平台的固定時間保證。舊基準的方法與限制見 [benchmark](../message_bus/benchmarks/README.md)。

### Manual wait validation / Manual 等待驗收

Validated on 2026-11-09. `module.context_wait` checks actual native notification
outcomes, deadlines, admission/quiescence, stop, degradation and reopen.
`module.context` uses a real Plugin to verify no-dispatch waits, callback rejection
and a Manual command/stop loop. `module.python` covers timeout conversion, GIL
release and command reentrancy. Installed Python SDK tests exercise both C++ and
Python wait linkage after relocation.

| Environment / 環境 | Results / 結果 |
| --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja | Debug and Release each passed 6/6: Context, Context wait, Runtime faults, Plugin, synchronous RPC and synchronous transport. SDK header smoke built. Debug CPython 3.13.12 passed 4/4 Python Module/message/service/RPC tests. |
| WSL Ubuntu 24.04.5 x64, GCC/libstdc++ 13.3.0, CMake 3.28.3, CPython 3.12.3, Release | The same six native and four Python cases passed: nine passed the first run; Context wait passed after correcting its FIFO fixture assumption below. SDK header smoke built. |
| Windows Debug, Windows Release, WSL Release | Final Context wait test repeated 20 times per configuration, all passed. Real cross-process `message_bus.module` also passed in each configuration. |
| Fresh Release SDK producers on Windows and WSL | Both `installed_consumer` and `installed_python` passed on each platform, including relocated C++/Python wait calls, custom messages, Bus/RPC/Param, ABI failure isolation, incremental generation, deployment repair and cleanup. |

Initial failures are retained rather than counted as successful runs:

- Windows initially reused an ABI-16 RPC fixture; rebuilding the omitted fixture
  target at entry 17 fixed the mismatch before the complete 6/6 rerun. A separate
  Python build attempt correctly rejected PATH selecting a different interpreter;
  using the existing CPython 3.13 environment restored the intended configuration.
- The first WSL Context wait test assumed a hint survives closure of the last FIFO
  reader. OSAL does not promise that. The corrected test signals at the native-wait
  boundary **after reopen** and requires a notification result, not fallback timeout.
  Production OSAL behavior was not changed to satisfy the test.

Local evidence (generated, not committed): Windows `build/wait-tests-{debug,release}.log`,
`build/wait-python-tests.log`, `build/wait-final-{debug,release}.log`,
`build/wait-repeat-{debug,release}.log` and `build/wait-sdk-release/`;
WSL validation workspace `wait-tests-first.log`, `wait-final-repeat.log`,
`wait-bus-tests.log` and `wait-sdk-release/`. Installed flow logs include
`INSTALLED_CPP_WAIT_OK` and `INSTALLED_PYTHON_WAIT_OK` before normal shutdown.

本輪 Windows Debug／Release 各六項、Windows Python 四項及 WSL 十項相關案例已通過；
WSL 首輪九項通過，修正 FIFO 測試假設後補過等待案例。最終等待測試三個組態各重跑
20 次，真實跨程序 Bus 測試亦各通過。Windows／WSL 乾淨 Release SDK 的 consumer 與
Python 兩套驗收完成，含搬移部署後 C++／Python 新介面及正常關閉。
首次 ABI fixture、Python PATH 配置與 FIFO 假設失敗紀錄保留，不以成功重跑抹除。

This is targeted acceptance, not a full-project CTest run or a new latency/CPU
benchmark. Native macOS, minimum MSVC 19.44, and Debug installed SDK were not
revalidated in this round. Recorder and Combat Bar workload acceptance remain
separate; see the root TODO and Bag README.

本輪為針對性驗收，未宣稱完整專案／新效能基準通過。原生 macOS、最低 MSVC 19.44
及 Debug installed SDK 本輪未重驗；Recorder 與 Combat Bar 負載改善另行驗證。

### macOS 來源補驗（2026-11-11）

[第二階段主報告](MACOS_SOURCE_FUNCTIONAL_VALIDATION.md)補齊目前 entry-19 的 Debug／Release
`module.context_wait`、Context／Runtime／Plugin、來源 Python／GIL、RPC 與 Recorder 功能證據。
上方歷史輪次未驗 Mac 的限制保留；後續[第三階段](../sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md)
已補乾淨 SDK-only C++／Python 搬移等待接線及正常停止。
本輪不量測延遲／CPU，也不宣稱 Combat Bar 負載驗收完成。

## Validation record (2026-10-28) / 本輪驗證

- Windows 11 x64 (10.0.26200), Ryzen 7 5700X3D, MSVC 19.51.36257 / toolset
  14.51.36231, SDK 10.0.26100.0, CMake 4.3.2, Ninja 1.13.2: complete Debug and
  Release builds passed. The 50 selected Common/OSAL Notification/Control/Bus/
  Module/Param/RPC/UI/Shell CTests passed in each configuration. After final
  admission/format/diagnostic refinements, Debug's 11 and Release's 12 focused checks passed; the latter includes the independent external SDK consumer build/deployment test.
- WSL2 Ubuntu 24.04.5 x86_64, GCC/libstdc++ 13.3, glibc 2.39, CMake 3.28.3,
  Ninja 1.11.1: Bus/Module **23/23** passed; the final eight affected tests also
  passed. This is WSL functional evidence, not a native Linux latency benchmark.
- Vue TypeScript checking passed. Chromium **6/6** passed, including yellow
  degradation, retained causes/target MID, recovery to green, red lifecycle
  priority, safe error text and manual refresh. Run browser tests only after the
  matching asset/server build finishes; a mixed-build trust fixture was rejected
  and passed after rerunning with matching artifacts. Trust checks were unchanged.

Windows Debug／Release 完整建置與各 50 項相關回歸通過；最後差異再驗證 Debug 11 項、Release 12 項（含外部 SDK 獨立建置／部署）。
Ubuntu 24.04 WSL 的 Bus／Module 23 項及最後差異 8 項通過。TypeScript 與 Chromium 6 項通過。
WSL 不當作原生 Linux 效能證據；瀏覽器須使用同版資產／伺服器，不能與重建並行混用。

Fault tests cover both publication paths, one failed destination while another
continues, immediate next-send retry, repeated-failure throttling, existing-name
reopening/catch-up, invalid receiver fallback without spinning, stop cancellation,
route deduplication/removal/subscriber-first registration, reader cleanup despite
route-encoding allocation failure, wire-capacity rejection/reuse, invalid identities,
unknown diagnostic codes, full cause chains and truncated/version-mismatched reports.
The existing process/Context tests cover batch/callback ownership, command dispatch,
stop/restart, callback failure, Python, RPC and Param regressions. Primitive pre-wait
retention/concurrent signals/cancellation evidence remains in the OSAL report.

故障測試涵蓋 push／loan、單一目標失敗不阻斷其他目標、重試／節流／恢復、失效 receiver
備援等待、停止取消、路由合併／退訂／先訂閱後發布、通知配置失敗不阻斷回收、容量拒絕後重用、
非法身分、完整錯誤原因鏈與截斷／版本錯誤。既有跨程序測試保留 batch、callback、Shell、
Python、RPC、Param 及生命週期回歸證據。原語的等待前通知與取消證據仍由 OSAL 文件維護。

### Outstanding validation / 待補驗證

Native macOS functional validation was added on 2026-11-02 after the
[portable notification snapshot repair](../message_bus/README.md#macos-notification-snapshot-blocker).
That record owns the mutex/lifetime contracts and snapshot tests; the
[Mac integration record](python/README.md#macos-snapshot-repair-validation) owns
the tested environment, Release scope and limited Debug coverage. Earlier
platform reports remain valid only for their recorded revisions. Native Linux
latency, pure idle CPU, very high-rate/topology-churn stress and maximum fan-out
resource limits have not been established by these tests. Retaining the fallback
means idle wake frequency is not promised to decrease. A native signal success is
only a hint acceptance, never proof of callback progress or durable delivery.

2026-11-02 已補通知快照修正及 macOS 功能驗證，範圍與限制見上述紀錄；不視為 Windows／Linux
已重跑這份 mutex 實作。本次依使用者決定不跑 benchmark。原生 Linux 延遲、
純閒置 CPU、高頻拓樸變動壓力與最大扇出資源極限仍待量測；保留補查不承諾降低閒置喚醒頻率。
通知成功不等於 callback 已執行或資料可靠交付。

### Performance evidence / 效能證據

The [measurement report](../message_bus/benchmarks/NOTIFICATION_RESULTS_2026-10-28.md) records 64 B–128 MiB, push/loan, 1,000 measured messages per case, 4 GiB shared budget, raw evidence paths and 0/1/8/32 receiver fan-out. All formal runs were clean; rejected large-payload pilots remain visible. On this Windows host, 1 MiB push/loan P99 was 0.5890/0.3668 ms versus the earlier approximately 16 ms polling observation. Pure idle CPU and native Linux/macOS latency remain outside this report.

量測報告保留所有正式與未合格試跑；正式組均零碰撞／零遺失／無完整性錯誤。此 Windows 主機的 1 MiB push／loan P99 為 0.5890／0.3668 ms，相較先前輪詢約 16 ms；不推廣為跨平台保證，純閒置與原生 Linux／macOS 延遲另待量測。

# Central scheduling cost comparison / 中央排程成本對照

This opt-in fixture compares current notification scheduling with a build-tree
copy of the same Manager using the pre-stage-three polling policy. It is not a
comparison against an old release. Protocols, providers, Plugin, compiler and
trace checkpoints are identical. No old-policy switch enters the SDK or Runtime.

這是同版程式、只替換中央排程的對照；不是不同版本產品的 benchmark。
`baseline.py` 驗證來源錨點後產生隔離副本，失配時停止建置，避免默默測錯版本。
歷史監督迴圈保留於 `polling_monitor.inc`，其餘實作直接取目前來源。

```text
cmake --build <Release-build> --target module_control_cost_notified module_control_cost_polling
python src/vinc/module_system/tests/control_cost/run.py <Release-build>/framework/module_system/tests <new-output-directory>
```

Configure a Release test build (`BUILD_TESTING=ON`) with the normal framework
toolchain and a compatible Python interpreter first. These targets are excluded
from the default build and add no normal CTest entries.

Run after other builds/tests finish. The runner alternates the two policies and
runs three repetitions of each with 4 and 128 callback subscriptions during
startup, followed by 10 seconds idle, then the same number of registrations via
Shell hooks. Each callback uses the unchanged Subscribe + EnableNotifications
protocol. Every request must succeed and the real Host must exit normally.
The 128 workload retains 256 callbacks after its second phase; it does not change
the configured capacity, remove requests, or relax the production startup budget.

等待其他建置／測試完成後才量測；每組交錯順序重跑三次。使用真實 Host、Plugin、
Socket 與 Bus；不是 mock，也不代表外部 MMDD 的 491 筆初始化通過。

Measurements / 量測邊界：

- `phases.csv`: central-process CPU time, wall time, supervisor passes/wait returns,
  threads, handles (Windows) or FDs (Linux), RSS and process-lifetime peak RSS.
- `parent.csv` / `child.csv`: fixed-capacity in-memory frame checkpoints, written
  after shutdown. Paired using generation/domain/operation/request IDs. Durations
  subtract timestamps only within one process, not across clock epochs.
- `results.json`: binary hashes, per-run phase data and P50/P95/P99 registration
  round-trip / central inbox delay. Small samples are descriptive, not tail SLAs.
- CPU is central only, includes instrumentation and resource sampling overhead;
  CPU percentage for one core is `100 * cpu_ms / wall_ms`. It excludes Host CPU.
- Pass/wait counts are supervisor events, not kernel context-switch counts. Idle
  health checks remain necessary; no zero-idle-wakeup claim is made.
- RSS includes all linked services and instrumentation; differences are not a
  direct measurement of ReceiveWake allocation. Cross-OS memory numbers are not
  equivalent. Windows CPU counters may report zero below their resolution.
- No hard-real-time, throughput, no-leak or external application readiness claim
  follows from this cost fixture. Behavioral regression and SDK relocation are
  separate acceptance gates.

The resource sampler currently supports Windows and Linux. macOS needs native
sampling and a fresh comparison; Windows/WSL results do not establish Mac costs.
Mac 功能與 installed SDK 驗收仍按根 TODO／CONTROL_WAKEUP 契約執行；本工具的
Mac 資源採樣尚未實作，不應假裝是已完成的跨平台效能測試。

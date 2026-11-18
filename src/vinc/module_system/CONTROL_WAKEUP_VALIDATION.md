# Central receive wakeup acceptance / 中央接收喚醒驗收

Status: stage-four Windows/WSL acceptance completed (2026-11-17). The
[2026-11-19 macOS report](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md) records the native follow-up. This report records
actual runs, including unsuccessful attempts; it does not replace the approved
[contract](CONTROL_WAKEUP.md).

## Scope and environment / 範圍與環境

- Same topic branch `codex/topic-registration-recovery`, base
  `fc27d2788e74a163ea597f4e736004dbcfc6d529` plus the uncommitted Bus repairs and
  receive-wakeup stages. The base commit alone does not identify the tested code.
- Windows 11 Pro 26200, Ryzen 7 5700X3D (8 cores / 16 threads), MSVC 19.51.36257 /
  toolset 14.51.36231, CMake 4.3.2, CPython 3.13.12.
- Same machine, WSL Ubuntu 24.04.5, GCC/libstdc++ 13.3.0, glibc 2.39,
  CMake 3.28.3, CPython 3.12.3. Linux evidence is WSL, not a separate bare-metal host.
- Functional builds/suites used overlapping machine load. Cost runs are separate,
  sequential, after the build/test jobs finish. The host is not an isolated lab.
- Production scheduling remains stage three. This stage adds opt-in cost fixtures,
  corrects a Param integration-test timing assumption and updates documentation.
  No public API, wire, capacity, startup deadline or priority was changed.

## Integration / 整合

| Platform | Debug related CTest | Release related CTest | Fresh SDK: consumer / App / Python |
| --- | --- | --- | --- |
| Windows | 69/69, 258.03 s | 69/69, 237.86 s | Debug 3/3; Release 3/3 |
| WSL Ubuntu 24.04 | 70/70, 95.21 s | 72/72, 129.10 s | Debug 3/3; Release 3/3 |

The final Param first-appearance guard was additionally rebuilt and verified with
five consecutive `param.process` runs in each OS/configuration (20/20 total).
Other production/test content in the table is unchanged by that last test fix.

Windows／WSL 的 Debug／Release 相關回歸共 280 次通過，四份乾淨 SDK 各完成
consumer、App、Python 三項驗收。Param 最後的首次出現等待補強另驗 20/20；
不是重新宣稱四組整合矩陣是在該測試補強後全數重跑。

The selected matrix is
`^(control\.|module\.|message_bus\.|rpc\.|param\.)`, not all project tests.
It covers real Module startup, normal/abnormal exit, first-failure priority,
shutdown, Context/pump, queue pressure, Bus topology/registration cleanup,
Param and typed/synchronous RPC. OS-specific and optional App test counts differ.

Fresh SDK producers use `VINC_INSTALL_SDK=ON`, `VINC_BUILD_APPS=OFF`,
`BUILD_TESTING=OFF`; independent consumer/application/Python fixtures install and
move the SDK, then build their own artifacts. App execution hides original build
and SDK paths, retains custom schemas/Bus/Bag/UI, and verifies shutdown. Python
also covers RPC/Param, native/Python parallel callbacks, ABI rejection, incremental
schemas/sources, deployment repair and disabled-feature cleanup. Installed private
ReceiveWake headers and static linking are exercised through the real Manager.

## Retained failures and corrections / 原始失敗與處理

1. Windows fresh Debug `consumer` exceeded its original 20-second guard on the
   first run; the same binary passed on retry and the complete consumer suite
   passed unchanged. The original run was under concurrent build load; that is
   context, not proof of the cause.
2. Windows fresh Debug relocated App exceeded its original 35-second guard after
   UI status. Module startup, custom Bus and Bag recording had succeeded. A direct
   retry encountered the old recording file and was **not** counted as acceptance.
   The complete fixture was rerun from its own clean work directory and passed
   with the original deadlines. UI HTTP timeout root cause is not independently
   established. Release App runs are serialized to avoid default-port overlap.
3. Tool sessions stopped during builds. Incremental output subsequently included
   an empty Linux `message_bus_recovery_app`, an unusable object and Windows PDB
   errors. Artifacts were retained and rebuilt. Three in-progress files (the new
   cost fixture/runner and SDK README) also contained zero bytes and were restored
   from recorded task content / the clean HEAD plus intended edits. The final
   source scan and builds passed; the interruption's cause is not established. Initial Linux failures were
   `Exec format error` before entering Bus/OSAL. They are not rollback failures.
   The Windows Param fixture also retained a November 10 Plugin; entry-version
   rejection was correct, and rebuilding that fixture fixed its mismatch.
4. A WSL `param.process` run failed `Public permits every Module`. The original
   test immediately read another Module's pump cache after a successful write.
   `Context::pump_control()` captures Param before dispatching command hooks;
   a concurrent write may land between those stages. Param explicitly promises
   central commit, not another Module's cache application. The test now checks
   `read_current()` for the committed value, then waits for cache convergence from
   the controller, within the existing test deadline. It does not pump recursively,
   sleep inside hooks, accept a wrong final value or modify Runtime behavior.
   Newly appearing watches are checked for presence before reading their cache;
   a missing registered watch is retried, while an unregistered watch is a failure.
   The original assertion did not log the stale numeric value, so that failed run
   alone is not a trace of every event in the race.
5. Initial Release selection had registered tests whose excluded targets had not
   been built. Missing executables are preparation failures, not test passes.
   The final run explicitly builds the selected executables and their dependencies.

The first SDK timeouts remain intermittent observations, not diagnosed production
bugs. Follow-up if they recur: record stage timings/stacks before changing any
public timeout. Retained failed App/build logs remain alongside successful
retries. The first consumer timeout output was observed in the tool transcript,
but its log was overwritten before retry preservation was added; it is not claimed
as a separately retained raw file.

## Cost method / 成本方法

See [fixture and reproduction](tests/control_cost/README.md). Both policies use
current providers/protocols and identical instrumentation. The polling baseline
reconstructs only the pre-stage-three supervisor loop; it is not an old release.
Three interleaved runs per policy and workload use 4 or 128 startup callback
subscriptions, 10 seconds idle, then 4 or 128 additional registrations.

CPU/resources refer to the central process; Host CPU is excluded. RSS is total
process memory, not ReceiveWake's allocation size. Supervisor passes/wait returns
are not kernel context switches. Same-process clocks and complete ID pairing are
required. Cost-smoke runs made under build load are excluded from final results.

### Results / 結果

All 24 real-process runs passed, including full request pairing and normal Host
exit. Tables show the median of three runs; latency columns are the median of
**per-run quantiles**, not pooled percentiles. Startup means `start_module()` to
Running, excluding Manager construction. Later registrations run through one
command hook per subscription. Four-sample P99 equals that run's maximum.

| OS / startup subscriptions | Startup: polling → notified (ms) | Later registrations: polling → notified (ms) | Subscribe queue P50 (ms) | Subscribe queue P99 (ms) |
| --- | --- | --- | --- | --- |
| Windows / 4 | 350.619 → 49.639 | 362.762 → 3.431 | 30.9340 → 0.0091 | 31.5755 → 0.0104 |
| Windows / 128 | 8118.620 → 122.963 | 11980.300 → 113.370 | 30.8815 → 0.0083 | 32.3542 → 0.0164 |
| WSL / 4 | 236.532 → 63.178 | 222.769 → 5.310 | 19.8832 → 0.0195 | 19.9061 → 0.0236 |
| WSL / 128 | 5226.820 → 175.321 | 7706.820 → 136.836 | 19.8689 → 0.0195 | 19.9370 → 0.0319 |

| OS / subscriptions | Startup CPU: polling → notified (ms) | Later registrations CPU (ms) |
| --- | --- | --- |
| Windows / 4 | 46.875 → 46.875 | 46.875 → 46.875 |
| Windows / 128 | 109.375 → 93.750 | 203.125 → 125.000 |
| WSL / 4 | 6.605 → 5.507 | 5.538 → 3.959 |
| WSL / 128 | 137.810 → 91.023 | 163.795 → 106.452 |

| OS / subscriptions | Idle CPU: polling → notified (ms / ~10 s) | Idle passes | Threads | Handles / FDs | Idle RSS (MiB) |
| --- | --- | --- | --- | --- | --- |
| Windows / 4 | 46.875 → 78.125 | 322 → 322 | 15 → 15 | 138 → 138 | 7.809 → 7.812 |
| Windows / 128 | 46.875 → 46.875 | 322 → 322 | 15 → 15 | 262 → 262 | 8.438 → 8.441 |
| WSL / 4 | 62.495 → 63.835 | 497 → 499 | 11 → 11 | 24 → 24 | 5.789 → 5.754 |
| WSL / 128 | 65.759 → 63.758 | 497 → 499 | 11 → 11 | 148 → 148 | 6.398 → 6.430 |

The old polling delay dominates the baseline inbox wait; notified reception
removes that delay in these workloads. It does not remove the health fallback.
Idle pass counts remain similar; at the idle checkpoint both policies have the
same thread and handle/FD counts within each matched run. The Windows command
phase sometimes has a different thread/handle snapshot after the much longer
polling run; that snapshot alone cannot identify the responsible subsystem.

Windows idle CPU is quantized in 15.625 ms increments. For the four-subscription
case the three polling samples were 62.500 / 31.250 / 46.875 ms, and notified
78.125 / 109.375 / 31.250 ms over about ten seconds. The median increase is
31.250 ms, roughly 0.31 percentage points of one core; these short samples do not
establish a persistent regression or prove zero overhead. Use longer repeated
idle sampling/profiling before attributing that difference to a component.

RSS is sampled, not an allocation/leak audit; lifetime peak RSS and all raw
phase/operation values remain in the JSON. No new steady-state thread/OS handle
was required by this design, but existing services still consume CPU and memory.
The notified work finishes sooner and may concentrate CPU into shorter bursts.

24 組皆完成註冊與正常退出。主要改善是移除「已進中央收件匣卻等下一輪輪詢」的延遲；
不是更改 Ready 期限、減少請求或改用平行註冊。閒置仍有健康補查。Windows 小負載的
CPU 樣本有波動，已保留較差數字，不據此宣稱零成本；記憶體採樣也不是洩漏證明。

Machine-readable evidence: [Windows](tests/control_cost/results/windows.json),
[WSL](tests/control_cost/results/wsl.json),
[source/evidence manifest](tests/control_cost/results/source-manifest.json).
Binary hashes identify the cost executables/Host/Plugin; the manifest identifies
changed source inputs on top of the stated base, including the final Param test.

## Reproduction and remaining platforms / 重現與平台邊界

```text
ctest --test-dir <build> -R "^(control\.|module\.|message_bus\.|rpc\.|param\.)" --output-on-failure
cmake --build <build> --target module_control_cost_notified module_control_cost_polling
python src/vinc/module_system/tests/control_cost/run.py <build>/framework/module_system/tests <new-directory>
```

Build every selected test target first; CTest does not build missing executables.
For SDK reproduction use the maintained `installed_consumer.cmake`,
`installed_application.cmake` and `installed_python.cmake` under `src/vinc/sdk/tests`,
with a new producer for each configuration. Do not run App fixtures sharing the
same default UI port concurrently on the same OS.

Local logs: `build/control-wakeup-stage4/{windows,wsl}-{debug,release}/`;
Windows SDK `build/control-wakeup-stage4-sdk-{debug,release}/`;
WSL SDK `/home/vinc-test/control-wakeup-stage4-sdk-{debug,release}/`.
Previous/interrupted logs retain distinct names. Raw cost traces remain in build
output; compact results and source hashes accompany this report.

macOS must rebuild this same content and run Debug/Release receive-wake, fair
scheduler/RPC-full, lifecycle/exit, Bus, Param, RPC and fresh installed SDK plus
relocation. The cost sampler requires native Mac implementation/measurement.
The [macOS report](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md) records this follow-up on the later integrated revision.
The external MMDD 491-registration / 30-second Ready acceptance remains separate.

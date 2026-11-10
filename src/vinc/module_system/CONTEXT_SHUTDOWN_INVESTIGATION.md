# Context shutdown investigation / Context 收尾競爭調查

Date: 2026-11-10. Branch: `codex/parallel-pump-contract`; base commit `6151381`,
with the existing uncommitted parallel-pump work. **Stages 1–2 completed:** the
historical investigation below is preserved; production repair and focused
validation are recorded under [Stage 2](#stage-2-repair-validation). Current
stage-3 results are maintained in the [regression report](CONTEXT_SHUTDOWN_VALIDATION.md);
native macOS acceptance is [handed off](MACOS_CONTEXT_SHUTDOWN_HANDOFF.md).

## Finding / 結論

Before the repair, Windows Debug reproduced a real Host shutdown race, followed by a test diagnostic
failure. `RuntimeGuard` closes the Bus transport before joining the watchdog that
receives Bus control frames. A watchdog iteration already past its `finished`
check can deliver a queued topology notification into the closed transport.
`RuntimeAccess::receive()` maps the rejection to Protocol; the watchdog exits with
72 (`central_lost`). Central records ProcessExited, restarts with a new MID, and
the test queries the obsolete MID and reports ModuleNotFound.

已重現正式 Runtime 的收尾競爭，並確認測試把原始故障遮成舊 MID 查詢失敗。
不是僅僅測試太慢，也不是多 Topic worker 才會發生。初始調查未修改正式 Runtime、
期限或斷言；後續第二階段修復另列於下方。原始第四階段那一次缺少內部診斷，不能百分之百追溯它的
唯一原因；本輪重現的是相同 `loss` 案例、相同對外錯誤及已確認的內部競爭。

## Evidence / 證據

Environment: Windows 11 x64 build 26200, MSVC 19.51.36257, CMake 4.3.2,
Ninja, Debug `/MDd`. Existing `module.context` CTest repeated **30/30**
(480.77 s); four direct-process lanes repeated ten times each, **40/40**.
These passing runs did not disprove the intermittent failure.

An isolated copy of `context_tests.cpp` runs the unchanged `loss` case repeatedly
with the original 700 ms stop timeout, one allowed restart, 100 ms restart delay
and 10 ms snapshot interval. It adds state/MID/error/exit tracing only. It links
the existing Debug libraries and initially launches the unchanged production
test Host and Plugin. Six lanes of up to 150 cases reproduced failures in four
lanes (requested MIDs 64, 62, 131, 121); the remaining two completed 150 each.
The failures exited with code 72, then restarted and completed normally with a
new MID. For example:

```text
case=loss requested_mid=64 elapsed_ms=221 mid=64 state=Stopping
case=loss requested_mid=64 elapsed_ms=283 mid=64 state=RestartWaiting
  primary=ModuleSystem / ProcessExited / ObserveExit; exit=72
case=loss requested_mid=64 elapsed_ms=720 mid=65 state=Stopped; exit=0
ModuleSystem / ModuleNotFound / WaitRunning
```

The first exit occurred before its 700 ms stop allowance expired. Increasing
that allowance is not a demonstrated fix.

Next, isolated Host/Bus copies add diagnostic records without changing branches,
policy, ordering or protocol. The Host marks its existing cleanup stages; Bus
records a rejected frame only when `Transport::closed` is already true. Five
separate Host processes captured the following matching evidence:

```text
BUS receive after closed: kind=5 domain=3 operation=1035 request=0
exit=72 finished=1 phase=5 session=2
error=ModuleSystem / BusFailed / Receive <- MessageBus / Protocol / Receive
```

Here kind 5 is Event; Bus wire operation 1035 (`0x040b`) is
`NotificationRoutesChanged`, not a payload message. Wire domain 3 is the Bus
control-protocol domain, not `ErrorDomain::ModuleSystem`. Diagnostic phase 5 means
the main thread already set `finished`, shut down Bus, closed Session and requested
watchdog stop; it has not yet proved watchdog exit. Session state 2 is Closed.
The Bus rejects on the first `closed` check, before decoding the body; this
evidence does not establish malformed payload bytes.

Review clarification (2026-11-10): these probes did not record the first call
that closed the transport or the pre-teardown main/completion result. Cleanup
phase 5 proves teardown reached that stage, not that main succeeded or that no
earlier send/request failure closed Bus. The lifetime-order defect and observed
receive-after-close are established; the staged repair's first step must capture
those missing causal details. See the [repair contract](CONTEXT_SHUTDOWN_FIX.md).

複審補充：現有紀錄未包含首次關閉來源與收尾前執行結果，不能把 finished／phase 5
等同正常成功退出。已確認的生命週期缺口仍成立；缺少的因果證據列入第一階段驗收。

Two separate diagnostic controls also establish the masking behavior: using the
existing `error` and `cleanup-stall` fixtures with the probe's success expectation
produces distinct HookFailed and StopDeadlineExpired failures, yet both finally
print the same ModuleNotFound. Those two runs intentionally fail; they are not
regressions in the original suite.

Log's native 10054 can occur in passing suites with intentionally terminated
fixtures. In the reproduced loss failures it accompanies abrupt Host exit; the
number alone is neither the root cause nor proof of a Log implementation fault.

Final probe counts (each parallel lane stops on its first failure):

| Probe | Successful cases | Failed cases | Interpretation |
| --- | ---: | ---: | --- |
| Existing Host, one 500-case lane | 500 | 0 | Passing repetition is insufficient |
| Existing Host, six lanes, at most 150 each | 674 | 4 | Real failure without Host/Bus instrumentation |
| Host cleanup diagnostics, six lanes, at most 300 each | 1,345 | 2 | BusFailed/Protocol after finished |
| Host and Bus diagnostics, six lanes, at most 150 each | 373 | 5 | All five record receive-after-close, operation 1035 |

All probe runners completed and managed children were checked for leftovers.
Counts are case invocations, not total process starts: a failed case can start a
second generation. The two intentionally failing control cases above are excluded.

## Code path / 程式路徑

1. `src/runtime/module_runtime.cpp`, `watch_central()`: receives a frame and checks
   `inbox.finished` once, then dispatches the frame without excluding concurrent
   RuntimeGuard teardown.
2. Same file, `RuntimeGuard::~RuntimeGuard()`: sets finished, calls
   `RuntimeAccess::shutdown(client)`, closes Session, requests stop, then joins.
   An atomic flag is not evidence that an already-admitted iteration has stopped.
3. `../message_bus/src/message_bus.cpp`, `Transport::receive()`: returns false
   immediately when closed. `RuntimeAccess::receive()` translates false to Protocol.
4. Watchdog wraps BusFailed/Receive and calls `_Exit(72)`. Its attempt to report
   the detailed failure uses the already-closing control Session, so central may
   only retain ProcessExited and the OS exit code.
5. `tests/context_tests.cpp`: successful cases also permit one restart; after
   observing the terminal snapshot, `wait_started(original_mid)` precedes the
   no-restart assertion. This hides the first failure behind an obsolete identity.

The same RuntimeGuard order exists at base commit `6151381`. This narrows the
code-history claim; it is not a full historical bisect or proof that every old
binary exhibits the race.

## Stage 1 controlled evidence / 第一階段受控證據

2026-11-10: stage 1 is complete; **the production defect remains unfixed**.
The new `module_shutdown_host` compiles the actual Runtime and Bus with
`VINC_SHUTDOWN_TESTING`. Only its two translation units receive that definition;
production targets have no hooks, source-location diagnostic overhead or gates.
Removing these conditional blocks reproduces the pre-stage-1 Runtime/Bus sources
in the existing WSL validation copy exactly. No production ordering was changed.

The fixture records the result of hooks and the result after Module destruction
and the completion exchange. Bus records only its first closed transition with
the actual caller function/line. A thread-local Guard marker distinguishes final
teardown from request/release/receive failure paths; those hooks do not gate while
holding the Bus mutex. Other close attempts do not overwrite the first source.

**Injection boundary:** after successful completion, the isolated watchdog replaces
one ordinary receive Timeout, after its finished check, with a valid empty
NotificationRoutesChanged event for an already-retired Publisher ID. It does not
replace real frames or transport failures, and it does not claim the event arrived
from the network. The actual dispatch, Bus validation, RuntimeGuard, watchdog
failure, process exit and central observation are used. Condition-variable gates
choose before-close or after-close ordering without fixed sleeps. Gate timeout
exits with test-only code 99, which cannot satisfy the defect assertion.

| Scenario | Observed result on Windows and WSL |
| --- | --- |
| Same valid event before Bus close | Accepted; successful completion, exit 0; no primary/cleanup fault |
| Same valid event after Bus close | Hooks/completion succeed; first close is FinalTeardown; BusFailed/Receive causes Protocol/Receive; exit 72 |
| Existing intentional hook failure | Hooks/pre-teardown result is HookFailed; exit 70 and central retains that original failure |

The after-close case records:

```text
hooks_result=ok
pre_teardown_result=ok
injected_notification=admitted_after_finished_check
first_close=FinalTeardown caller=...RuntimeAccess::shutdown... line=1252
bus_receive closed=1 injected=1 valid_empty_route=1 operation=1035
watchdog_bus_protocol=1
watchdog_exit=72 ... BusFailed/Receive <- MessageBus Protocol/Receive
```

Central recorded ProcessExited/ObserveExit in these runs. The test also permits
the exact BusFailed/Receive with Protocol/Receive cause if best-effort fault
delivery wins the Session-close race. It always requires actual exit 72; timeout
or arbitrary failures are rejected. All cases disable restart to retain the first
generation. Controlled cases use a 5 s stop budget and 2 s gate failure guard;
the unmodified natural loss case retains its 700 ms stop budget. Production
defaults and existing test deadlines remain unchanged.

| Environment | Focused regression | Controlled repeat | Final-source controlled check |
| --- | --- | --- | --- |
| Windows 11 x64 build 26200, MSVC 19.51.36257, CMake 4.3.2, Debug /MDd | Context / shutdown_evidence / runtime_faults: 3/3 | 10/10 | Pass |
| WSL2 Ubuntu 24.04.5 x86-64, GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, Release | Same three tests: 3/3 | 10/10 | Pass |

The final-source check follows a test-only initialization-warning correction and
the precise central-report alternative above. Windows additionally ran 100 natural
loss cases without frame injection: all exited normally. This does not negate
the prior intermittent failures or estimate production frequency. The newly
controlled evidence establishes the causal flaw; original intermittent runs still
lack their own first-close records. Windows Release, full-project regression and
native macOS repair acceptance remain for later stages.

第一階段已在 Windows Debug／WSL Release 證明：正常 hooks／完成協定成功後，
RuntimeGuard 首次關閉 Bus，再派送有效路由通知會導致 exit 72。同一通知提前派送
則 exit 0。測試通過代表「精確重現既有缺陷」，不是「修復完成」。Windows 的新診斷
自然觀察 100 次未重現，保留此結果，不誇大推估發生率。Mac 仍須原生驗收。

Local artifacts under ignored `build/`:

- `shutdown-stage1-controlled-final.log`, `shutdown-stage1-repeat.log`,
  `shutdown-stage1-regression.log`, `shutdown-stage1-natural-100.log`.
- `shutdown-stage1-wsl-controlled-final.log`, `shutdown-stage1-wsl-repeat.log`,
  `shutdown-stage1-wsl-tests.log`, and Windows/WSL `shutdown-stage1-*-build*.log`.
- `shutdown-stage1-evidence/`: pre-fix source copies and SHA-256 manifest, preserved
  for a later old-order comparison. The tracked test source is reproducible;
  local artifacts are supplementary and are not installed or uploaded.

Reproduce on a configured test build:

```text
cmake --build <build-dir> --target module_shutdown_tests module_runtime_fault_tests
ctest --test-dir <build-dir> -C Debug -R "^module\.(shutdown_evidence|context|runtime_faults)$" --output-on-failure
ctest --test-dir <build-dir> -C Debug -R "^module\.shutdown_evidence$" --repeat until-fail:10 --output-on-failure
```

Select the build's actual configuration; WSL used Release. For natural observation,
run `module_shutdown_tests <shutdown-host> <context-plugin> 100`. Its exit status
means the observation/cleanup completed, not that all observed exits were normal;
inspect the explicit `exit72` count and per-process diagnostics. Those stage-1
results used the preserved pre-fix revision; stage 2 below changes the maintained
test to require safe ordering rather than expect failure on repaired code.

<a id="stage-2-repair-validation"></a>

## Stage 2 repair validation / 第二階段修復驗證

Date: 2026-11-10. Same branch/base commit as above, with authorized stage-2
working-tree changes; no commit or push. Source hashes are saved locally in
`build/shutdown-stage2-manifest.json` to distinguish this state from stage 1.

### Changes and lifecycle audit / 改動與生命週期審查

- `RuntimeGuard`: preserve user cleanup and completion/failure exchanges; cancel
  adapter waits, set finished, request watchdog stop and join it, then close Bus
  and Session. Join holds no inbox/adapter lock. No new thread, deadline, ABI or
  runtime setting was introduced.
- `Transport::receive`: return Result directly. Entry after closure is
  Closed/Receive; malformed input is Protocol/Receive even when its rejection
  sets closed. RuntimeAccess keeps the original shutdown-on-error and exception
  classification boundaries. Notification dispatch is never broadly ignored
  merely because teardown started.
- `context_tests`: successful cases have zero retries; check original MID,
  retained faults and actual exit before wait_started. Failure-policy cases keep
  their explicit one-restart budget. Diagnostics identify the case, requested and
  retained MID, primary/cleanup cause, state and exit.

| Adjacent adapter | Audit / 保留理由 |
| --- | --- |
| Param, locks, queries | stop requests cancellation; it does not destroy the adapter or its mutex/pending state. Keep stop before join to release waiters; objects remain alive through join. |
| RPC Host | stop sets stopped and wakes waiters; progress checks stopped. Host destruction remains after watchdog join. |
| Pump notification | pump is quiesced before close. Sender close/wake is serialized by its mutex and inactive state; watchdog diagnostics/progress retain a live HostNotification object. |
| Log / Plugin | Log adapter outlives receive dispatch; its existing bounded fence/close is separate. Plugin code remains loaded through Module destruction, receiver join and service destruction. |

### Controlled proof / 受控證據

The isolated Host compiles the real dispatcher/Bus with test-only checkpoints.
A valid NotificationRoutesChanged for a retired Publisher (id 1, zero targets)
is admitted after the watchdog's finished check, following successful hooks and
completion exchange. The gate releases when Guard is about to join, **not after
Bus closes**. Thus the corrected lifetime can complete without a circular test
dependency. Final retirement must follow the receiver-exited checkpoint.

Four maintained cases cover notification before teardown, notification admitted
during teardown, a truncated notification at the same boundary, and a real hook
failure. The first two exit 0; malformed data retains BusFailed/Receive with
Protocol/Receive and exit 72; hook failure retains HookFailed and exit 70. Test
gate timeout (exit 99), arbitrary errors and cleanup failures never count as proof.
For malformed data, central may retain either the precise fault report or actual
ProcessExited because Session closure races best-effort reporting; the Host trace
must still contain the exact cause chain.

An isolated Windows mutation restores **only RuntimeGuard's old order**, keeping
the new Closed classification and current test hooks. Running the same
`--case during-teardown` fails with this exact evidence:

```text
hooks_result=ok
pre_teardown_result=ok
receiver_exited_before_bus_shutdown=0
first_close=FinalTeardown
bus_receive closed=1 injected=1 valid_empty_route=1
BusFailed/Receive <- Closed/Receive
exit=72; restarts=0
Shutdown evidence test failed: same notification is accepted before teardown
```

No gate timed out. This demonstrates that reclassifying the error alone cannot
fix the lifetime defect. The production source was never temporarily reverted;
the mutation has separate sources, objects and executable under ignored build/.

### Results and scope / 結果與範圍

| Environment | Focused tests | Controlled repeat | Old-order mutation |
| --- | --- | --- | --- |
| Windows 11 x64 26200; MSVC 19.51.36257, CMake 4.3.2; Debug /MDd | 9/9 | 10/10 (four cases per run) | Rejected with exact receive-after-close evidence |
| WSL2 Ubuntu 24.04.5 x86-64; GCC/libstdc++ 13.3, CMake 3.28.3, Ninja 1.11.1; Release | 9/9 | 10/10 (four cases per run) | Not repeated in this stage; pre-fix controlled proof is retained in stage 1 |

The nine tests are `module.context`, `module.shutdown_evidence`, `module.process`,
`module.runtime_faults`, `module.parallel_stop`, `message_bus.error_protocol`,
`message_bus.error_process`, `message_bus.cleanup` and
`message_bus.notification_snapshots`. They cover actual parent loss/heartbeat
loss, abnormal exit, stalled hooks/callbacks, cancellation/drain and malformed
input as well as normal completion. They do not constitute full-project,
Windows Release, installed-SDK or native macOS acceptance for this repair.

Commands on each configured test build:

```text
cmake --build <build-dir> --target module_shutdown_tests module_context_tests module_integration_tests module_runtime_fault_tests module_parallel_stop_tests message_bus_error_protocol_tests message_bus_error_process_tests message_bus_cleanup_tests --parallel 4
ctest --test-dir <build-dir> -C Debug -R "^(module\.(shutdown_evidence|context|runtime_faults|process|parallel_stop)|message_bus\.(error_protocol|error_process|cleanup|notification_snapshots))$" --output-on-failure
ctest --test-dir <build-dir> -C Debug -R "^module\.shutdown_evidence$" --repeat until-fail:10 --output-on-failure
module_shutdown_tests <shutdown-host> <context-plugin> --case during-teardown
```

Use the build's actual configuration (WSL was Release). Local logs:
`shutdown-stage2-windows-tests.log`, `shutdown-stage2-windows-final.log`,
`shutdown-stage2-windows-repeat.log`, `shutdown-stage2-wsl-tests.log`,
`shutdown-stage2-wsl-repeat.log`, `shutdown-stage2-old-order.log`, and
`shutdown-stage2-*-build*.log` under build/. The last Windows/WSL focused runs
include the final test diagnostic/argument changes; repeated controlled runs
exercise the same runtime/checkpoints. The comparison script
`build/shutdown-stage2-old-order.py` and generated `windows-msvc-debug/shutdown-old-order/`
are local evidence only. Nothing is uploaded.

第二階段已修正正式收尾順序，並驗證 Closed／Protocol 分類不會掩蓋真正故障。
兩平台各 9/9 針對性測試、10/10 受控重跑通過；Windows 舊順序對照只恢復順序就
重現 exit 72。這是因果對照，不是靠延長 timeout 或重跑碰運氣。完整回歸、自然
短命 Module 長跑及 Mac 原生驗收保留第三階段，原始偶發紀錄的證據限制仍然成立。

## Repair plan / 修復計畫

The [Host shutdown repair contract](CONTEXT_SHUTDOWN_FIX.md) owns the target
lifecycle order, error classification, three implementation stages and acceptance
gates. This report retains historical evidence. Stages 1–2 are complete;
stage 3 was authorized on 2026-11-10. See the
[regression report](CONTEXT_SHUTDOWN_VALIDATION.md) for current Windows/WSL coverage
and the [Mac handoff](MACOS_CONTEXT_SHUTDOWN_HANDOFF.md) for pending native acceptance.

修復契約與三階段開工步驟集中於上述文件，本報告保留原始調查證據。
第一、二階段完成；第三階段已獲授權，實際 Windows／WSL 結果集中於驗收報告，Mac 待原生驗收。

## Local artifacts / 本地證據

All executable probes and logs remain under ignored `build/`; no installation,
remote upload or production library replacement was performed.

- Original failure: `build/parallel-stage4-debug-regression.log`.
- Existing-suite repetition: `build/context-investigation-repeat.log` and
  `build/context-investigation-lane-<lane>-run-<run>.log`.
- Unchanged-Host probe: `build/context-investigation-loss-lane-<lane>.log`.
- Host diagnostics: `build/context-investigation-diagnostic-lane-<lane>.log`.
- Host plus Bus diagnostics: `build/context-investigation-bus-lane-<lane>.log`;
  detailed process files `build/windows-msvc-debug/context-investigation/host-*.log`.
- Probe construction: `build/context-investigation-build.py` and
  `build/context-investigation-host.py`; generated copies and binaries are in
  `build/windows-msvc-debug/context-investigation/`. They reuse the existing
  Ninja compile/link requirements and write separate objects/executables.

The diagnostic runs perturb scheduling, so failure proportions are not production
failure-rate estimates. The original binary also reproduced before Host/Bus
instrumentation. Those initial probes were Windows-only; subsequent stage-1 WSL
coverage and stage-2 production repair are recorded separately above. These
historical probes do not establish full-project or native macOS acceptance;
the later regression report owns current coverage.

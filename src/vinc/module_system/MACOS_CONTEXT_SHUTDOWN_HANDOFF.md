# macOS Host shutdown acceptance / macOS Host 收尾驗收交付

Status: **Native Mac C++ Host/Context Debug/Release acceptance completed**; see the
[Mac report](MACOS_CONTEXT_SHUTDOWN_VALIDATION.md) for results and limits.
原生 Mac 第一階段已完成；來源 Python／GIL 與 SDK-only 專項交付仍屬後續階段。
Prepared 2026-11-10. The original checklist below remains a reproduction guide.
[Repair contract](CONTEXT_SHUTDOWN_FIX.md) owns behavior;
[investigation](CONTEXT_SHUTDOWN_INVESTIGATION.md) preserves earlier evidence.

## Source and prerequisites / 來源與前置條件

- Validate the **same final repair content** used by Windows/WSL. The original
  handoff used uncommitted `codex/parallel-pump-contract` work based on `6151381`;
  **6151381 alone does not contain the repair**. It was published as `820d4ba` and
  merged into `aab5731` with an identical tree. Native Mac acceptance locks
  `aab573100a231aa4b4e960fb9e9b49c47ad9d1d8`, not a moving branch name. Record
  the actual commit/status; any later source transfer requires its own hashes/diff.
- Build Host, Plugins and tests together (current Plugin entry 19). Do not mix
  old SDK/Plugin objects. Use distinct Debug and Release directories.
- Record `sw_vers`, `uname -m`, CMake/Ninja, compiler and SDK versions, selected
  standard library, and Python ABI if Python tests are enabled. Follow existing
  toolchain contracts; do not silently switch compilers to obtain a pass.
- Confirm `node --version` and `npm --version` in the environment that launches
  CTest. A cached absolute Node path in the producer does not make Node available
  to a freshly configured external consumer. The WSL first run exposed this
  distinction; use an existing compatible installation in the test process PATH,
  and preserve any original environment failure separately from a Runtime fault.
- Before integration tests, inspect listeners on 31410/31411. Do not stop an
  unrelated App without permission. Run Debug and Release tests sequentially;
  independent CTest invocations do not share RUN_SERIAL protection.

來源必須包含本次 RuntimeGuard、Bus 分類及新測試；原交付時尚未發布，現已於上述 commit 驗收。
先確認收到的實際版本，再測試。既有 Mac FD／退出觀察通過紀錄仍有效於各自版本，
但不能拿來代替本次收尾修復驗收，也不要將後續新功能的 pending 寫成那些舊功能「未驗證」。

## Build and regression / 建置與回歸

From the repository root, use the validated native compiler environment. These
commands build the bundled C++ App with its current Kconfig and explicitly enable
SDK installation/integration tests, matching the Windows Debug and WSL Release
regression trees (the existing Windows Release tree omits installed-SDK tests).
They do not enable Python implicitly; use the existing Kconfig flow for that coverage.

```sh
cmake -S src -B build/context-shutdown-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVINC_INSTALL_SDK=ON
cmake --build build/context-shutdown-debug --parallel 4

cmake -S src -B build/context-shutdown-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DVINC_INSTALL_SDK=ON
cmake --build build/context-shutdown-release --parallel 4
```

For **each** directory (Debug first, then Release), retain command exit status
and complete stdout/stderr. Use its real configuration if using a multi-config
generator. Do not pipe to `tee` without preserving the originating exit code.

```sh
ctest --test-dir build/context-shutdown-debug -C Debug -j1 \
  -R '^(module\.|message_bus\.|rpc\.|param\.|control\.)' \
  --output-on-failure --output-log build/mac-shutdown-debug-focused.log
ctest --test-dir build/context-shutdown-debug -C Debug -j1 \
  --output-on-failure --output-log build/mac-shutdown-debug-full.log
ctest --test-dir build/context-shutdown-debug -C Debug -j1 \
  -R '^module\.shutdown_evidence$' --repeat until-fail:10 \
  --output-on-failure --output-log build/mac-shutdown-debug-controlled-repeat.log
ctest --test-dir build/context-shutdown-debug -C Debug -j1 \
  -R '^module\.context$' --repeat until-fail:5 \
  --output-on-failure --output-log build/mac-shutdown-debug-context-repeat.log
```

Repeat with release/Release and separate logs. Confirm at least these tests are
registered: `module.shutdown_evidence`, `module.context`, `module.context_wait`,
`module.parallel`, `module.parallel_stop`, `module.topic_scheduler`,
`module.process`, `module.runtime_faults`, `module.manager_faults`,
`message_bus.error_protocol`, `message_bus.error_process`, `message_bus.cleanup`
and `message_bus.notification_snapshots`. An empty selection is not a pass.
The broader pattern additionally covers RPC/Param/control and recovery contracts.
If using a Python-enabled App, rebuild its binding and run its registered
`module.python*` / `rpc.python` tests; report disabled coverage as disabled.

## Controlled assertions / 受控案例應證明什麼

`module.shutdown_evidence` runs the real Host dispatcher/Bus in a test-only
instrumented executable. Its macro must not be enabled on production libraries.
No fixed sleep is used to decide the critical ordering: bounded gates coordinate
admission and the point immediately before join.

| Case | Required result |
| --- | --- |
| before-close | Valid notification accepted, hooks/completion successful, exit 0, no restart/fault. |
| during-teardown | Notification admitted after finished-check but before teardown dispatches while Bus is still open; receiver exits **before** final Bus retirement; exit 0. |
| malformed | Truncated notification is still rejected: Host records ModuleSystem BusFailed/Receive with MessageBus Protocol/Receive and exit 72. Central may retain that report or actual ProcessExited; no arbitrary error is accepted. |
| hook-error | Genuine HookFailed, exit 70; must not become successful completion. |

Gate timeout / exit 99 is a **test failure**, not a reproduction. Keep the entire
trace with MID, hooks_result, pre_teardown_result, first_close, bus_receive,
watchdog_exit, primary/cleanup error and actual exit. The maintained test already
asserts the required outcomes and performs bounded Manager shutdown before
removing its own temporary files.

已在 Windows 隔離副本只恢復舊收尾順序，證明有效通知會收到 Closed 並 exit 72，
新測試正確失敗。Mac 不需要改正式程式來重新引入缺陷；若另做對照，使用隔離目錄、
保留源碼差異，不能把測試故障或任意非零退出當成缺陷證據。

## Short-lived Module repetition / 短命 Module 重跑

Use `ctest --test-dir <build> -N -V -R '^module\.shutdown_evidence$'` to obtain
the actual test executable, Host and Context Plugin paths; do not guess `.so`
versus `.dylib`. Run that command with a trailing `100` (each configuration):

```text
<module_shutdown_tests> <module_shutdown_host> <module_context_helper> 100
```

Preserve its complete output. **Exit status 0 only means observation and cleanup
completed**; require the summary `natural observations=100 exit72=0`, and every
case's actual exit 0 / restart count 0 / no primary or cleanup failure. This
fixture runs the original loss workload without frame injection. The repeated
`module.context` also exercises the ordinary production Host, finite pump batches,
registration/topology changes and unsubscribe, whereas the natural probe includes
diagnostic hooks. Neither observation count estimates production failure rate.

Verify no child created by these tests remains after completion. Preserve owned
temporary resources if cleanup fails for inspection; do not delete unrelated
shared-memory files or kill similarly named user Modules.

## Return report / 回報格式

1. Commit/worktree identity, OS/CPU/toolchain/configuration and build command.
2. Per-configuration build result, focused/full CTest counts including skipped or
   disabled tests; preserve first failure logs before any rerun.
3. Controlled repetition 10/10, Context repetition 5/5, natural count/exit72 count,
   first-generation exit/fault evidence and cleanup result.
4. Classify any failure: lifecycle regression, test/launcher environment, existing
   unrelated issue, or still unexplained. Do not increase a timeout or weaken an
   assertion just to turn it green. New architecture issues require discussion.
5. Update this file with the actual results or link a native report; update the
   owning README and root TODO. Only close the Mac item when evidence exists for
   both configurations. Missing coverage must remain explicit.

驗收交付不授權任意重構。若失敗，先留下第一份 log／實際錯誤，再分析原因；不要只交
一份「重跑通過」。完整證據與結果交回後，才能把 TODO 的 Mac 待驗移除。

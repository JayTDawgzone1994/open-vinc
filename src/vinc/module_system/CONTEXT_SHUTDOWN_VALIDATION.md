# Host shutdown regression / Host 收尾修復第三階段驗收

Date: 2026-11-10. **Windows/WSL acceptance completed / Windows／WSL 驗收完成**.
Native macOS is explicitly deferred
to the [Mac handoff](MACOS_CONTEXT_SHUTDOWN_HANDOFF.md), not claimed as tested here.
The [repair contract](CONTEXT_SHUTDOWN_FIX.md) owns behavior and scope; the
[investigation](CONTEXT_SHUTDOWN_INVESTIGATION.md) preserves the original failures
and controlled old-order/new-order proof.

## Tested content / 受測內容

- Branch `codex/parallel-pump-contract`, base `61513819b7157ab02af1aafc20b5c5ab95f46d45`,
  plus the existing uncommitted RPC/wait/parallel-pump work and shutdown repair.
  The base commit alone does not contain the repair. No commit or push in this stage.
- Stage 3 changes documentation only. It does not alter production timeouts,
  failure priority, restart policy or tests to obtain a pass.
- Windows and WSL normalized hashes match for the nine Runtime/Bus/test sources
  recorded in `build/shutdown-stage3-source.json`. WSL's existing validation copy
  was synchronized to current repository source before building; its generated
  Python-enabled configuration remains distinct from the bundled Windows App.
- Builds are incremental full builds of existing test trees. SDK integration
  fixtures create their own external consumers, installation prefixes and relocated
  packages; this does not claim a fresh producer checkout/build.

## Environments and results / 環境與結果

| Environment | Toolchain / configuration | Related component suite | Full registered suite | Context repeat | Controlled repeat | Natural loss workload |
| --- | --- | --- | --- | --- | --- | --- |
| Windows 11 x64 build 26200 | MSVC 19.51.36257, CMake 4.3.2, Ninja 1.13.2; Debug | 47/47 | 156/156 | 5/5 | 10/10 | 100/100 normal exits |
| Same Windows host | Same toolchain; Release; SDK installation tests disabled in this tree | 47/47 | 149/149 | 5/5 | 10/10 | 100/100 normal exits |
| WSL2 Ubuntu 24.04.5 x86-64 | GCC/libstdc++ 13.3, glibc 2.39, CMake 3.28.3, Ninja 1.11.1; Release; CPython 3.12.3 | 53/53 | First run 170/171; PATH-corrected follow-up 1/1 | 5/5 | 10/10 | 100/100 normal exits |

The Windows bundled configurations disable Python Modules. Debug enables
`VINC_INSTALL_SDK` and exercises installed consumers (including Python); Release's
existing tree has `VINC_INSTALL_SDK=OFF` and therefore omits those seven installed
SDK cases. Release still includes the source/external consumer tests. WSL Release
enables SDK installation coverage. Separate existing Python-enabled
source-App trees, with CPython **3.13.12**, were rebuilt for this repair:
Debug **7/7**, Release **7/7** (`module.python`, messages, runtime_metadata,
runtime, services, parallel, and `rpc.python`). These use the Windows `/MT`
binding closure while the central/native Plugins retain `/MD` or `/MDd`.

The related suite selects `module.*`, `message_bus.*`, `rpc.*`, `param.*` and
`control.*`. It includes Manual/FrameworkLoop, serial/multiple Topic workers,
callback drain, parent/heartbeat loss, stuck hooks, real protocol faults and
recovery. The full suite includes SDK/App composition and deployment as actually
registered in each build. Different totals reflect configuration/platform coverage;
do not present them as identical test lists.

Repeated `module.context` runs use the ordinary production Host and retain
first-generation diagnostics; `module.shutdown_evidence` runs four controlled
cases per repetition. The natural probe has no injected frame, runs the original
loss workload and reports `exit72` separately from its command exit status. A
natural observation is accepted only with exit72=0, every actual process exit 0,
zero restarts and no primary/cleanup fault. Counts do not estimate field failure rate.

All three natural logs were checked individually: 100 actual exit-0 records,
zero restarts, no primary/cleanup failure or gate timeout, and 100 confirmations
that the receiver exited before Bus retirement in each configuration. The final
Windows process inventory found no process under the four validation build trees;
the WSL build-path process inventory was also empty. The probe additionally checks
Manager shutdown before removing its own temporary resources.

## Commands and artifacts / 命令與紀錄

```text
cmake --build <build> --parallel 4
ctest --test-dir <build> -C <config> -j1 -R "^(module\.|message_bus\.|rpc\.|param\.|control\.)" --output-on-failure
ctest --test-dir <build> -C <config> -j1 --output-on-failure
ctest --test-dir <build> -C <config> -j1 -R "^module\.context$" --repeat until-fail:5 --output-on-failure
ctest --test-dir <build> -C <config> -j1 -R "^module\.shutdown_evidence$" --repeat until-fail:10 --output-on-failure
module_shutdown_tests <shutdown-host> <context-plugin> 100
```

Build roots: `build/windows-msvc-debug`, `build/windows-msvc-release`, and
`/home/vinc-test/python-module-deploy-20261105/build` (WSL copy). Windows Python
source consumers: `build/parallel-python-debug` and `build/parallel-python-release`.

Local evidence under ignored `build/`:

- `shutdown-stage3-{windows-debug,windows-release,wsl-release}-results.json`:
  actual command, exit status and elapsed time for each stage.
- Same prefix with `-build.log`, `-focused.log`, `-full.log`,
  `-context-repeat.log`, `-controlled-repeat.log`, `-natural-100.log`.
- `shutdown-stage3-python-results.json` and Python Debug/Release build/test logs.
- `shutdown-stage3-source.json`, WSL synchronization list, and final manifest.
- `shutdown-stage3-final-manifest.json`: verified source hashes, commands/results,
  natural-run assertions and SHA-256 of local evidence; `shutdown-stage3-process-check.log`
  records the final process inventory result.
- One initial Release build was intentionally interrupted before tests, solely to
  serialize Windows fixed-port validation. Its log is preserved as
  `shutdown-stage3-windows-release-build-interrupted.log`; it is not a test failure.
  A separate build-only pass then completed successfully; Release tests wait for
  the Debug runner. No unrelated user process was stopped.
- The Release orchestration wait expired after one hour before any Release test
  started. The runner was requeued to keep fixed-port suites sequential. This
  changes no framework/test timeout; `shutdown-stage3-release-orchestration.log`
  records the scheduling event separately from test results.

All evidence remains local. Any failing first run must stay in these records;
reruns cannot erase the original outcome. Existing stage-1/2 evidence is retained.

## Environment deviation and remaining acceptance / 環境差異與尚待驗收

WSL first full run: `sdk.external_consumer` failed during fresh configuration
because Node was absent from the CTest process PATH. The producer's cache points
to an existing Node 24.19.0 installation; that cache is not inherited by the fresh
consumer. No Runtime code executed in the failing test. The follow-up passed
**1/1 in 383.35 s**, using only
that existing installation's bin directory in its child-process PATH, without
installing software or changing framework/toolchain policy. Preserve both the
original full log and `shutdown-stage3-wsl-external-consumer-path.log`.

Native Mac Debug/Release: follow [the handoff](MACOS_CONTEXT_SHUTDOWN_HANDOFF.md)
and return actual logs/results before closing the root TODO item. Prior Mac
FD/spawn, process-exit and Plugin classification reports remain evidence for their
own revisions; this pending item does not revoke those results.

本輪只處理收尾修復驗收與文件，不改正式 Runtime。Windows Debug 156/156、Release
149/149；WSL 首輪 170/171，唯一 Node PATH 環境失敗補驗 1/1 通過，不改寫成一次
171/171 全過。三組各完成 Context 5 次、受控案例 10 輪及自然短命 Module 100 次，
沒有非預期重啟／exit 72，也未留下測試子程序。Mac 依使用者指示交付，不能把其他
平台通過當成 Mac 已驗證。尚未 commit／push。

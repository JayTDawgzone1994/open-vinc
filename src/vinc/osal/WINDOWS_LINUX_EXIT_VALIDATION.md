# Windows / Linux exit-progress acceptance (2026-11-03)

## Scope / 範圍

Source baseline: `9f0ec0bc9f1b3a7ef752c067adcd9de8427252a5` (develop, PR #24).
This round validates the shared observer/native-interface changes in
[Process exit progress](PROCESS_EXIT_PROGRESS.md), the repaired Plugin tests and
the example deployment fixture. Production code and maintained tests are unchanged.
Publication integrates develop `f313072` (Mac SDK acceptance documents only); the
707 tested non-document inputs still match after integration.
The earlier [Mac results](MACOS_EXIT_VALIDATION.md#current-acceptance-status) retain
their original failures and later targeted passes.

本輪以同一版來源補驗 Windows 與 WSL Linux，不修改正式 Runtime、OSAL、期限或測試斷言。
來源的 707 個非 Markdown 輸入先記錄 SHA-256；Linux 原生檔案系統副本逐檔比對一致。
測試使用 Git 外的 CMake 父專案，透過正式 `add_subdirectory`／`vinc_add_app` 組裝
`ExitValidation`，選入 Python 與兩個 C++ 使用者範例，執行未修改的 smoke.py。
官方預設 App 與所有預設 test targets 亦建置。

## Environment / 環境

| Item | Windows | Linux |
| --- | --- | --- |
| OS / CPU | Windows 11 Pro x64, build 26200; AMD Ryzen 7 5700X3D, 8 cores / 16 threads | Ubuntu 24.04.5 x86_64, WSL2 kernel 6.18.54.1-microsoft-standard-WSL2, same CPU |
| Compiler / library | MSVC 19.44.35229, toolset / STL 14.44.35207 | GCC / libstdc++ 13.3.0, glibc 2.39 |
| CMake / Ninja | 4.3.2 / 1.13.2 | 3.28.3 / 1.11.1 |
| CPython | 3.13.12 | 3.12.3 |
| Configurations | Separate fresh Debug / Release trees | Separate fresh Debug / Release trees on WSL ext4 |

Python binding build/run use the same interpreter in each environment. The helper
App uses a custom absolute interpreter on Windows and PATH/System on Linux.
Pinned dependency versions and framework build policies remain unchanged.
Linux results describe this native Linux userspace under WSL, not deployment on
Ubuntu 22.04, a separate physical Linux host, or other glibc/libstdc++ versions.

## Results / 結果

All four final runs passed, with zero CTest failures, disabled cases or skips.
The mounted-source build failure below remains a separate unresolved build issue.

| Check | Windows Debug | Windows Release | Linux Debug | Linux Release |
| --- | --- | --- | --- | --- |
| Fresh configure / all default targets | Passed | Passed | Passed | Passed |
| Targeted exit / Plugin / Manager / Runtime tests | 19/19 | 19/19 | 19/19 | 19/19 |
| Selected source/native/integration regression | 107/107 | 107/107 | 114/114 | 114/114 |
| Fresh installed consumer / application / Python | 3/3 | 3/3 | 3/3 | 3/3 |

| Command duration, seconds | Windows Debug | Windows Release | Linux Debug | Linux Release |
| --- | --- | --- | --- | --- |
| All-target build | 323.87 | 399.21 | 226.61 | 227.44 |
| Targeted CTest | 42.63 | 39.21 | 14.61 | 14.02 |
| Regression CTest | 420.93 | 414.91 | 201.28 | 192.41 |
| Installed SDK CTest | 704.88 | 761.15 | 686.52 | 591.44 |

Durations include command overhead and are not performance comparisons. Windows
and Linux drivers ran concurrently on the same physical CPU. Each platform ran
its own configurations and test groups sequentially.

The targeted tests are the 14 `osal.process_exit_progress.*` cases plus
`module.plugin`, `module.failure_order`, `module.manager_faults`,
`module.runtime_faults`, and `app.plugin`. The same cases run again within the
broader regression; counts must not be added as unique coverage.

回歸選取 OSAL、Module、App、Shell、Param、RPC、Message Bus、Control Channel、
OutputHub、Bag 與範例；每組 CTest 循序執行。包含真正的 process handle／pidfd／waitpid
路徑，以及取消、逾時、退出回收、Plugin 分類／安全拒絕／析構、受控故障保存順序、
assertion unwind 的先回收後刪檔、無法確認時保留檔案，及 C++／Python 範例的多次中央啟停。
Linux 額外執行 POSIX FD／descriptor／通知原生案例；平台專屬項目使數量不同。
Mac 專用 `osal.process_exit_macos` 不適用這兩平台，沒有冒充略過後的通過。

The shared progress cases compile the real observer against a controlled native
boundary; they are not native kernel failure injection. Native Process tests and
App/SDK children provide separate real-OS evidence. SDK fixtures install and move
the new producer's package and build external consumers; App/Python flows exercise
custom messages, callbacks, Bus/RPC/Param, failure isolation and normal shutdown.
These are fresh full-source producers with SDK export enabled, not SDK-only producers.

This is selected regression, not a full-project CTest rerun or a benchmark. It does
not repeat Mac's full native stress counts or prove arbitrary scheduling/leak freedom.
Existing compiler warnings remain in logs.

## Initial failures and fixture preparation / 初次失敗與環境準備

1. Linux initial source was `/mnt/d/code/open-vinc`, with a new ext4 build tree.
   Parallel-6 Debug build failed before functional testing: two Shell Tcl resource
   deployments exceeded the existing 60-second lock wait. The initial Release build
   was deliberately interrupted, then the serial Debug retry was also interrupted
   to move validation to an ext4 source tree. Neither interruption is a passing build.
   See the [Tcl outstanding issue](../../../library/tcl/README.md#待處理事項).
2. The ext4 snapshot initially omitted `.git` pointers; both fresh configure attempts
   were correctly rejected by fmt's pinned-revision check. Read-only submodule gitdir
   pointers were then supplied and new build trees used. No version check was disabled.
3. Final Linux source tree: `/home/vinc-test/open-vinc-exit-validation-20261103`;
   builds: `/home/vinc-test/build/exit-native-verified-{debug,release}`.
   UI npm packages and read-only submodule revision metadata reference the original
   checkout; compiled framework/upstream source is in the ext4 snapshot. Source hashes
   match; only the external App profile and parent project are validation inputs.

Moving the source does not fix the mounted-source deployment timeout. It separates
that known build issue from the runtime acceptance; the limitation remains recorded.

## Reproduction and evidence / 重現與證據

Git-excluded evidence: `build/exit-platform-validation/`. `run.py` records Windows
commands, times and exit codes; `native_linux.py` records the final Linux runs.
`source/CMakeLists.txt` holds the external composition; the selected profiles are
`build/param-stage1/source/{windows,python}.conf`, copied for the Linux snapshot.
`source-inputs.json` contains the original revision and non-document source hashes.
Initial and retry logs use different prefixes and are retained.

For each fresh configuration, the drivers run configure, all-target build, JSON test
inventory, targeted CTest, selected regression and the three installed-SDK tests.
CTest uses `--parallel 1 --output-on-failure --output-junit <file>` and these filters:

```text
^(osal[.]process_exit_progress[.]|module[.](plugin$|failure_order$|manager_faults$|runtime_faults$)|app[.]plugin$)
^(osal[.]|module[.]|app[.]|shell[.]|param[.]|rpc[.]|message_bus[.]|control[.]|output_hub[.]|bag[.]|examples[.])
^sdk[.]installed_(consumer|application|python)$
```

Logs/JUnit live beside `{windows,linux-native-retry}-results.json`; nested SDK logs
remain under each producer's `vinc-source/sdk-*-test` directories. They are local
evidence, not repository artifacts. Mac's newer Param entry-13/wire-v3 clean-SDK
acceptance subsequently passed in its [own round](../sdk/MACOS_PARAM_VALIDATION.md);
it is not counted as part of these Windows/Linux runs.

Final checks: the 707 non-document source hashes still match, and process inspection
scoped to this round's build/deployment paths found no remaining test children.
Individual lifecycle tests also assert confirmed exits; the final process list is
additional bounded evidence, not a general leak-free or worker-join guarantee.
`windows-final-audit.json` and `linux-final-audit.json` retain those final checks;
the latter also rechecks all 707 files in the Linux source snapshot.

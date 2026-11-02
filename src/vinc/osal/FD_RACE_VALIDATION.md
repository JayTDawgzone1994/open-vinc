# FD creation/spawn review (2026-11-02) / FD 建立與程序啟動交叉驗證

## Status / 狀態

**Investigation only; no production fix. / 本輪僅驗證及分析，未修改正式實作。**

最新 `develop` 已 fast-forward 至 `627c409`。Windows 與 WSL Linux 的基本回歸通過，
但受控測試確認 Linux 也存在內部 FD 建立／搬移與 spawn 的競爭，不能將一般測試通過
解讀為此漏洞已排除。macOS 的 CLOEXEC 標準串流缺失、退出觀察逾時另立問題，
不與 FD 競爭混為一談。

This record supplements the [OSAL outstanding issues](README.md#待處理事項).
The owner subsequently confirmed the repair scope. The [repair contract and stage plan](FD_SPAWN_PLAN.md) now own those decisions; the results below remain pre-fix evidence.

## Baseline and evidence / 基準與證據

| Item | Recorded environment or source |
| --- | --- |
| Windows / Linux baseline | `627c409`, fresh common/OSAL builds from this checkout |
| Windows | Windows 11 x64, build 26200; MSVC 19.44.35229, toolset 14.44.35207; Debug / Release |
| Linux | Ubuntu 24.04.5 under WSL 2, x86_64; kernel 6.18.54.1-microsoft-standard-WSL2; GCC/libstdc++ 13.3, glibc 2.39; Debug / Release |
| Linux build tools | CMake 3.28.3, Ninja 1.11.1, Python 3.12.3 |
| Supplied macOS evidence | `599e7af11147a0c2e678799b7b31507e1c24d663`; macOS 26.3.1(a), arm64 Apple M1 Pro; Apple Clang 21.0.0, SDK 26.5; Debug / Release |

The supplied archive is
`open-vinc-macos-fd-all-in-one-20261102.tar.gz`, obtained from the owner's
[Google Drive file](https://drive.google.com/file/d/1BNFLtSbRSgAtT8nGZWB8h2e02q9pD2eR/view).
Its SHA-256 is
`271490acf31e85c1f7eb25a30e24df4b90825bb8b29d52b2365a440393a9a502`.

- Read `evidence/REPORT.md`, the frozen harness, actual suite summaries and the
  unmodified exit-timeout sample. All **73 selected files covered by the outer
  checksum list matched**; this is not a verification of every archive member.
- Use `evidence/diagnostic/` for the final macOS instrumented results and
  `evidence/unmodified/` for the uninstrumented control. The archive also contains
  exploratory failures; its directory named `final/` is not the final clean harness.
- Compared 136 original common/OSAL file hashes with the current Git tree:
  126 match. Differences concern documentation, Python selection/path handling,
  associated errors/tests, and Windows launch error-mode handling. The POSIX spawn,
  relocation, common process observer and macOS monitor sources relevant here match.
- macOS results are supplied native evidence, **not rerun on this Windows machine**.
  WSL results establish the stated Linux environment, not every Linux deployment.

Local raw evidence and disposable fixture sources are under
`build/fd-race-review/` (ignored by Git). Linux's ext4 work tree is
`/home/vinc-test/open-vinc-fd-review-20261102`.
The collected `linux-evidence/raw-results.tar.gz` retains raw per-case results;
`linux-evidence/results/*/summary.json` retains the classifications and commands.
Windows final results are in `windows-final-Release/` and `windows-final-Debug/`.
The initial `windows-results-*` directories retain the rejected fixture run.
The compact local handoff archive is
`build/fd-race-review/windows-linux-evidence-20261102.zip`; it includes fixture sources,
logs, raw results and a content checksum manifest, without rebuildable binary trees.

## Basic regression / 基本回歸

| Checks | Windows | Linux / WSL |
| --- | --- | --- |
| Selected existing App / Module / OSAL regression | 9/9 | 10/10 |
| Fresh standalone common/OSAL CTest, Release | 28/28 | 31/31 |
| Fresh standalone common/OSAL CTest, Debug | 28/28 | 31/31 |

The first row reused previously validated binaries: changes between their tested
baseline and `627c409` are documentation-only. The standalone rows are new builds.
This is a focused verification, not a new full framework / SDK acceptance run.

## Linux controlled results / Linux 受控測試結果

Each column below reports one configuration; Debug and Release gave the same counts.
`Defect reproduced` means the fixture demonstrated bad product behavior, **not a
passing product contract**. These counts are controlled schedules, not natural failure rates.

| Suite, per configuration | Cases | Contract pass | Defect reproduced |
| --- | ---: | ---: | ---: |
| Missing stdio × Inherit / Null / Capture | 144 | 144 | 0 |
| Same matrix with retained stdio marked CLOEXEC | 144 | 144 | 0 |
| 1 / 63 / 64 Pipe bindings plus stdout/stderr Capture | 24 | 24 | 0 |
| Resource / schedule matrix, double spawn and Pipe EOF | 213 | 81 | 132 |
| Notification keepalive endpoint schedules | 6 | 0 | 6 |
| Repeated FileLock schedules | 100 | 0 | 100 |
| Seeded selection of controlled resource schedules | 100 | 46 | 54 |
| Native API control, separate from OSAL | 600 | 0 | 600 |
| Unmodified OSAL, normal spawn / wait / shutdown | 3,000 | 3,000 | 0 |

No supervisor, oracle or observation timeouts occurred in these final Linux suites.
Each normal-spawn control runs in a fresh fixture process with a fresh ProcessService.

The macOS harness was copied and reviewed before use. Linux changes were confined
to the disposable fixture: use glibc close-from actions instead of Darwin's spawn
flag, expect Linux native error source `4`, and expect Linux same-fd spawn dup2
to preserve CLOEXEC stdio. The double-spawn Linux check verifies a valid second
stream and no extra descriptors; the single-spawn cases verify resource identity.

Four observers were inserted only into an isolated OSAL copy: the three relocation
helpers and the point after stdio actions are recorded. They preserve errno and
do not alter ownership, flags or native return values. Condition-variable gates
control ordering; sleeping is not used to manufacture the race. The separate
unmodified copy has an empty instrumentation patch and no forced observer include.

Concrete Release evidence:

- `Release-resources/00000`: the child inherited the **FileLock file identity** as
  fd 0 (`dev=2096, ino=249012`), instead of a missing-stream null fallback.
- `Release-resources/00001`: stdio actions captured a low FD, then relocation closed
  it before spawn. Result: `Osal / SystemError / Spawn`, Linux errno **EBADF (9)**.
- `Release-resources/00002`: numeric FD reuse delivered the replacement resource
  to the child. Checking whether an integer FD is valid cannot establish identity.
- `Release-resources/00212`: after the legitimate Pipe writer closed, the read
  timed out at 100 ms while the child remained alive. After terminating and reaping
  that child, the read returned EOF. The child had retained an unintended writer.
- Native controls also wrote a marker to the accidentally inherited/reused file;
  these establish native mechanics, not an additional OSAL API result.

Resource coverage includes FileLock create/open, SharedMemory create/open, Pipe,
Socket/accept, process/Pipe/Socket monitors, and Notification directory/endpoints.
The inherited identity and EOF evidence do **not** independently prove a FileLock
lease persisted after its owner died; that consequence still needs a dedicated test.

## Windows comparison / Windows 對照

Windows does not allocate native HANDLEs into POSIX's fd 0/1/2 slots. The current
spawn backend duplicates each selected standard HANDLE, supplies NUL for absent
standard handles, and passes an explicit `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`.
These are distinct mechanisms; there is no valid translation of the POSIX race
that merely substitutes numeric Windows CRT descriptors.

Per configuration, the comparison exercises:

- **288** combinations: eight missing-standard-handle masks × eighteen valid stdio
  modes × inheritable/non-inheritable source handles. Verify native file identity,
  null fallback, capture bytes, and rejection of an unrelated inheritable file HANDLE.
  Concurrent OSAL FileLock/open, SharedMemory, Pipe and Socket churn continues during
  spawning. This is stress coverage, not deterministic scheduling inside DuplicateHandle.
- **24** explicit Pipe-binding cases (1/63/64 endpoints, eight closed-CRT-stdio masks),
  validating slot identity, simultaneous stdout/stderr capture and EOF.
- **100** EOF controls: create an unrelated inheritable native pipe, launch a live
  child, close the parent's writer, and observe `ERROR_BROKEN_PIPE` **before** child
  termination. No accidental writer retention in these cases.
- **3,000** normal spawn/wait cycles, each with a fresh ProcessService, using an
  unmodified Windows backend. This uses one supervisor process, unlike Linux's
  fresh fixture process per case; it is not an identical reproduction of the Mac run.

All **288 matrix + 24 binding + 100 EOF cases passed in both Debug and Release**;
the 3,000 fresh-service spawn/wait cycles also passed in each configuration.
No macOS-style exit-observation timeout was reproduced. Cold-to-final process HANDLE
counts were 74 → 76 in both fresh-service runs; the earlier reused-service controls
were 77 → 77. An additional Release profile of 1,500 fresh-service cycles sampled
77 handles with the active service at every 250-cycle boundary, then 76 after final
shutdown and again after 100 ms. No growth across those samples was observed. These
coarse counts do not establish universal leak freedom or identify the two retained
handles; their allocation origin was not traced in this investigation.

This does not guarantee correctness if an external caller concurrently closes or
replaces a GetStdHandle source during DuplicateHandle. No such external mutation
was injected, and no private OSAL lock could automatically coordinate arbitrary code.

The first fixture incorrectly called exclusive `FileLock::create()` on the same
path every churn iteration. All 288 matrix cases in each initial configuration
therefore failed at the fixture's repeated create. The corrected fixture creates
once and repeatedly calls `open()`, preserves the exception diagnostic, and uses
new final result directories. The initial failures are not OSAL race evidence.

## What the macOS evidence establishes / macOS 報告判讀

The reported findings are supported by the selected raw results:

1. **FD creation/spawn race:** reproduced on both macOS and this Linux environment.
   Exact outcomes differ: Darwin can lose CLOEXEC stdio or reject some kernel
   descriptors, while Linux can inherit the resource. Both are contract violations.
2. **CLOEXEC stdio without a race:** macOS recorded 69 failures / 144 cases in each
   configuration; Linux recorded 144/144 passing in each configuration. A fix must
   not assume Darwin's same-fd spawn dup2 behaves like glibc's implementation.
3. **Exit observation without FD races:** macOS's unmodified control recorded
   Debug 3/3,000 and Release 1/3,000 timeouts. Release sample `02729` shows the same
   owned child already in zombie state at 0.254 seconds, followed by a five-second
   `wait_until()` timeout; `try_wait()` immediately recovered exit code 0.
   This is not explained by slow child execution. The root cause is still unknown.

Review `src/process.cpp::{run_monitor,observe,Process::wait_until}`, the publication
of a newly spawned record, and `src/macos/monitor.cpp::{watch_process,wait,wake}`.
In particular, capture the ordering of watch registration, NOTE_EXIT consumption,
waitpid(WNOHANG), record publication and wake delivery. A one-shot notification /
reapability timing issue is a **hypothesis**, not a diagnosed cause. Do not hide the
failure by increasing the timeout or declaring Linux/Windows success a Mac fix.

## Repair contract / 修訂契約

The original proposals were reviewed and accepted after this investigation.
The [FD/spawn repair contract and implementation plan](FD_SPAWN_PLAN.md) owns
confirmed behavior, cross-image coordination, initialization limits and four stages.
The macOS exit-observation root cause remains unconfirmed; no production fix or
post-fix validation is claimed by this report.

本報告保留修復前證據；已確認的修訂契約與開工進度集中維護於上述文件。

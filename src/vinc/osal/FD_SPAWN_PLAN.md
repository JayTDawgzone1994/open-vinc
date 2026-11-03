# FD/spawn repair contract and implementation plan / FD／spawn 修訂契約與開工步驟

## Status and ownership / 狀態與文件責任

**Design confirmed on 2026-11-02; stages 1–2 are implemented. See the stage-specific evidence below.**
The owner accepted the synchronization/snapshot cost, shared coordination across
Host/Plugin OSAL copies, and the initialization/external-code boundaries below.
This document records those decisions and the work sequence; it is not itself
authorization to modify implementation. Follow [AGENTS.md](../../../AGENTS.md).

**第二階段已接入 POSIX FD 建立與 spawn；2026-11-03 macOS 原生補驗及退出調查結果見 [Mac 報告](MACOS_FD_VALIDATION.md)，後續退出修復的實作與待驗範圍見 [退出進度紀錄](PROCESS_EXIT_PROGRESS.md)。** 本文件是本次修訂契約與階段進度的主要維護位置；
[驗證報告](FD_RACE_VALIDATION.md)保存修復前的證據，不將已重現誤記為已修復。
各階段需依使用者的開工授權執行；新增重大架構問題仍須先討論。

## Confirmed scope / 已確認範圍

| Work | Contract |
| --- | --- |
| POSIX FD creation/spawn races | Coordinate directly managed acquisition and inherited-stdio snapshots. Cover Linux and macOS. |
| macOS CLOEXEC stdio | Deliver inherited streams through owned copies, without relying on same-fd spawn dup2 behavior. |
| Multiple OSAL images | Host and its participating Plugins share one coordinator within that process. Keep static linking. |
| macOS exit observation | Investigate and repair separately, preserving sole child-reaping ownership and existing API behavior. The repair and controlled tests are implemented; subsequent native exit checks passed, and the two Mac integration-test gaps have since been repaired and verified in separate follow-ups. Historical failures remain recorded; Windows/Linux reruns are still pending. See the [design](PROCESS_EXIT_PROGRESS.md) and [Mac acceptance](MACOS_EXIT_VALIDATION.md). |
| Windows | Preserve native HANDLE duplication and the explicit inheritance list. Validate shared integration without adding an unnecessary POSIX resource lock. |

本次不改 Message Bus／Context 的資料傳輸、heartbeat、重啟政策或一般 Module 公開服務 API；
不調整 Python `/MT` 或既有工具鏈／CRT 相容性契約，不新增 registry、背景執行緒或自動服務探索。
FileLock 原生 unlock 失敗後的持鎖狀態仍是另一項待辦。

## FD acquisition and spawn / FD 建立與啟動契約

1. Coordinate the full native acquisition → relocation → required flag setup →
   publication or failure cleanup interval. Locking only the relocation helper is insufficient.
2. Inventory all directly managed FD producers: Pipe, Socket/open/accept,
   FileLock/create/open, SharedMemory/create/open, Notification directories/endpoints,
   process/Pipe/Socket monitors, and pidfd. Include failure and retry paths.
3. While using the same coordinator, snapshot the streams requested by spawn.
   Each valid inherited stream receives an owned, close-on-exec duplicate. A missing
   stream is recorded for a child-only null-device action; do not alter parent stdio.
4. Place snapshot source FDs outside the complete child Pipe bootstrap target range,
   as well as fd 0/1/2. Derive the boundary from actual handoff requirements, not a
   guessed fixed FD number. Capture and explicit Pipe bindings retain their own ownership.
5. Once the snapshots and required actions are safely owned, release coordination
   before native spawn. Do not borrow the original stdio numbers again. Keep copies
   alive until the spawn attempt completes, then release them on every exit path.
6. Closing/reusing an original FD after the snapshot must not redirect the child to
   a different resource. This protects identity/lifetime, not independent file offsets
   or a frozen copy of the underlying stream's data.

同步範圍必須消除內部 FD 暫占標準串流的窗口；子程序使用自己持有的來源副本。
缺少串流時只調整子程序，不偷偷補開父程序的 stdin／stdout／stderr。

### Why at most three copies / 為什麼最多三個副本

The bound is the fixed standard-stream set: stdin (0), stdout (1), stderr (2).
There is at most one new inherited-stdio snapshot copy per stream per spawn.
Use named constants or a fixed array to express this invariant.

**This is not the total FD budget for spawn.** Capture pipes, explicit Pipe handoff,
monitoring resources and other concurrent spawns require their own descriptors.
This bound is not tunable and must not become a `config.hpp`, Param or Kconfig setting.

**三個只限制這次新增的標準串流快照副本。** 不是整個 spawn 只能用三個 FD，也不是
整個程序的總額度；多個 spawn 各自持有副本。此數量不列為可調參數。

### Waiting, lock order and failures / 等待、鎖順序與失敗

- Do not add this coordination to ordinary Socket/Pipe transfer or shared-memory
  payload access. The accepted cost is acquisition and launch preparation.
- Do not hold it across accept readiness waits, DNS resolution, native spawn,
  process-exit waits, user callbacks, logging/formatting or other service dispatch.
  Native acquisition itself may have OS/filesystem latency; no hard-time bound is promised.
- `try_*` behavior must not become an indefinite internal wait. Deadline/cancellation
  operations must account for coordination waiting within their existing contract.
  Do not introduce busy retry loops when a socket is ready but coordination is busy.
- Document the actual ordering against service/record/endpoint locks before wiring
  producers. Avoid recursive acquisition through helpers; make ownership explicit.
  Failure cleanup must not reopen the low-FD race or invert the lock order.
- Expected acquisition/snapshot failures use `Result`, with the public operation
  at the outer layer and meaningful lower-level/native causes retained. Never fall
  back to an unprotected original FD on duplication failure.
- Release partial snapshots and failed unpublished resources without deleting
  resources merely opened from an existing name. Preserve Pipe admission/ownership
  contracts and confirm failure followed by successful retry.
- Binding/lifecycle programmer errors and runtime resource failures remain distinct;
  do not introduce framework exceptions or broaden existing exception policy.

## Shared coordinator and Plugin lifetime / 共用協調器與 Plugin 生命週期

OSAL owns the generic coordination and borrowing interface; Module System owns
framework integration. OSAL must not depend on Module, Context, MID or Concurrency.
Exact new C++ names/signatures remain implementation details to specify in stage 1;
this document does not reserve new public API names.

```text
Central process: its own coordinator

Each native Module Host process:
  Host-owned coordinator
    + Host OSAL copy
    + participating Plugin OSAL copies
```

This is in-process coordination, not an interprocess lock shared by all Modules.
Standalone OSAL use retains a default coordinator. Independent library images do
not automatically share defaults; explicit integration joins the common owner.

1. Host establishes ownership before Plugin application work. It supplies a narrow,
   validated borrowing interface during Plugin initialization, before Module creation.
   Only opaque state and required operation entries cross this boundary; Plugin
   does not free Host synchronization state or its allocations.
2. Keep existing Common `RuntimeServices` fatal-only. Pass coordination through
   separate Plugin initialization wiring; no Common-to-OSAL reverse dependency.
3. Do not replace a coordinator after affected operations have started. Detect/reject
   unsafe late binding and duplicate/replacement attempts instead of changing locks
   beneath active resources. No automatic image discovery or symbol interposition.
4. Plugin global/static constructors must not create affected OSAL resources or
   start related work before framework initialization. Move such work into the
   initialized Module lifecycle. This restriction concerns affected resource paths,
   not all harmless OSAL value/parsing operations.
5. Stop/join Plugin work before unloading. Host state and callback code outlive all
   borrowing operations and Plugin cleanup, including unloading-time destructors.
   Do not revert to a private coordinator while shared work may still exist.
6. Reject failed initialization for that Module and retain central/other-Module
   operation. If safe unloading cannot be established, end that Host rather than
   unload code with live work. Preserve bounded failure reporting and supervision.
7. Increment Plugin entry version for the changed initialization contract and reject
   incompatible artifacts explicitly. Rebuild connected Host/Plugin/SDK artifacts.
   Existing matching-toolchain/standard-library/CRT/layout restrictions still apply.
   Do not change unrelated wire versions unless their actual format changes.

中央與各 Host 各有自己的協調器；同一 Host 中的 OSAL 副本才共用。保留靜態連結，
由框架接線，不要求一般 Module 使用者自行取得或管理鎖。初始化前的資源建立限制與
不安全綁定必須有可理解的錯誤，不能只留下隱藏的未定義行為。

## Guarantee boundary / 保證邊界

Coverage is limited to participating OSAL directly managed resource paths.
Arbitrary third-party `open/close/dup2`, direct standard-stream/handle replacement,
libc resolver descriptors and dynamic-loader internal descriptors are not
automatically coordinated. A wrapper calling such a library does not make its
internal FD activity protected. This is not a sandbox or interception of native APIs.

使用者已接受此邊界。不得將「OSAL 直接管理的建立路徑」宣稱成「程序內任何函式庫的 FD
都安全」，也不得為擴大承諾而在 loader callbacks／第三方任意程式碼外層持鎖。

## Separate macOS exit investigation / macOS 退出觀察獨立工作

The unmodified macOS control demonstrates an exited zombie child followed
by `wait_until()` timeout and immediate `try_wait()` recovery. The subsequent
[2026-11-03 native traces](MACOS_FD_VALIDATION.md) confirm a consumed NOTE_EXIT
followed by waitpid(WNOHANG) returning zero, then an indefinite monitor wait.
Watch registration, record publication and user wakes succeeded in the fully
traced failing sample. Subsequent [step 1 controlled reproduction and lifecycle design](PROCESS_EXIT_PROGRESS.md)
is complete: the original observer deterministically stalls in Debug and Release.
Step 2 subsequently implemented the repair and passing maintained regressions. Mac native/integration checks and the two test repairs are recorded in the [current acceptance summary](MACOS_EXIT_VALIDATION.md#current-acceptance-status); Windows/Linux reruns remain pending.

Keep OSAL as the sole reaper. Use bounded observation records and test-owned
process cleanup; preserve failed samples. The owner subsequently accepted 50ms follow-up
only while consumed exit notifications still need observation, preserving caller deadlines.
The recorded exit-repair authorization covers the implementation, controlled contracts and subsequent Mac acceptance. Do not turn this into
permanent polling, increase public timeouts or treat other-platform passes as a fix.
New major lifecycle trade-offs still require discussion before implementation.

## Implementation stages / 分階段開工步驟

A stage's evidence must identify its source revision,
platform, toolchain, build configuration and limitations. Intermediate stages need
not be independently releasable; do not label partial integration as the complete fix.

| Stage | Implementation scope | Acceptance evidence |
| --- | --- | --- |
| 1. Coordinator and integration | Generic OSAL ownership/borrow contract and lock ordering; Host/Plugin initialization and lifetime; entry version and SDK wiring | Standalone use; two independently linked test libraries demonstrably share one owner; invalid/late binding, initialization rollback and unload lifetime; no Common reverse dependency or cross-image freeing |
| 2. POSIX FD acquisition and spawn | Inventory/wire all producers, retries and cleanup; inherited-stdio copies and child actions; keep Windows native behavior | Linux/macOS controlled acquisition schedules; all stdio/CLOEXEC modes; FD reuse; concurrent spawns; 1/63/64 Pipe handoffs; partial snapshot failure/exhaustion/retry; parent stdio unchanged; EOF and dedicated FileLock owner-death checks; lock/deadline/cancellation tests |
| 3. macOS exit observation | Native ordering diagnosis, then the evidence-supported monitor/observer repair | Uninstrumented normal-spawn controls in Debug/Release, plus regression covering the diagnosed ordering; no competing reaper; preserve timeout/cancellation/shutdown semantics |
| 4. Integration and documentation | Source and installed SDK; real App/Host/Plugin lifecycle; finalize contracts and remove resolved TODOs only with evidence | Windows/Linux/macOS Debug/Release; source and installed consumers; old entry rejection, startup failure isolation, repeated start/stop and cleanup; record remaining gaps |

### Stage acceptance rules / 階段驗收規則

- Convert useful disposable reproducers into maintained component tests. A fixture
  classifying a defect as reproduced is not a passing regression test after repair.
- Redesign scheduling hooks for the new lock boundaries; do not intentionally wait
  inside the shared lock for another operation that must acquire that same lock.
- Test partial duplication failures at each snapshot position and FD exhaustion;
  retain native diagnostics and verify later retry succeeds without leaks or stale slots.
- Distinguish real OS failures from substituted backends, forced schedules and
  fixture errors. Preserve raw evidence and bound/clean up every owned test process.
- Use native Mac evidence for Mac behavior; Windows and WSL cannot substitute for it.
  Coordinate Mac execution on the same topic content without losing existing evidence.
- Functional tests must establish resource identity, output bytes, EOF/lifetime and
  exit status, not merely successful spawn or compilation.

### Progress / 進度

- [x] Baseline investigation and Windows/Linux comparison recorded.
- [x] Owner decisions and standard-stream copy bound confirmed; this contract written.
- [x] Stage 1: coordinator and Host/Plugin integration implemented; Windows/WSL evidence below.
- [x] Stage 2: POSIX acquisition/spawn implementation and Windows/WSL/macOS FD validation; independent Mac exit-observation failures remain recorded.
- [x] Stage 3: macOS exit-repair checks and the two integration-test follow-ups completed. Native exit checks passed 6,624/6,624; original full integration is Debug 151/153 and Release 152/153. The Plugin test repair now has controlled ordering/cleanup coverage and native Mac validation; the subsequent Debug/Release full suites each pass 153/154. The example fixture was then repaired and passed focused Mac Debug/Release CTest (1/1 each); the full suite was not rerun after that final script-only change. See the [Mac report](MACOS_EXIT_VALIDATION.md) and [Module investigation](../module_system/MACOS_PLUGIN_CLASSIFICATION.md). Historical failures remain recorded; Windows/Linux validation remains separate.
- [ ] Stage 4: final three-platform repair acceptance. Earlier source/SDK runs are recorded, but Windows/Linux must rerun the subsequent exit repair, and the retained Mac integration failures need resolution.
- [x] Stage 4 Windows/WSL: Debug/Release source and installed-SDK acceptance recorded below.
- [x] Stage 4 macOS: native `a19c0d4` Debug/Release integration passed; see the separate exit-stress failures in the Mac report.

**Owner deferral (2026-11-02):** Skip native macOS work for now: both the new
stage 2 repair acceptance and stage 3 exit-observation investigation remain open
in the [root TODO](../../../TODO.md#osal-macos-標準串流與退出觀察). Existing Mac
baseline evidence remains valid; this is not a resolution or a passing acceptance.
Resume Debug/Release verification on the same topic branch and commit as the
other platforms. The deferral itself did not authorize stage 4.

**使用者決定暫緩（2026-11-02）：** 本輪跳過 Mac 原生工作，第二階段修復驗收及
第三階段退出觀察調查保留待辦；既有 Mac 重現證據仍有效，不標記完成。

**Stage 4 authorized on 2026-11-03:** run Windows/WSL Debug and Release source
and installed-SDK acceptance. Native macOS stage 2/4 acceptance and the stage 3
investigation remain deferred in TODO; do not mark three-platform acceptance complete.

**Subsequent Mac authorization on 2026-11-03:** the owner resumed native stage 2/4
acceptance and stage 3 investigation, with documentation only: no production fix
and no benchmark. The [Mac report](MACOS_FD_VALIDATION.md) owns this round's results
and supersedes the earlier deferral. The subsequently authorized exit-repair steps 1–2 and remaining validation are recorded separately in [exit progress](PROCESS_EXIT_PROGRESS.md).

**Exit-repair step 3 subsequently authorized:** native Mac exit, full source App/Python
and clean SDK verification are recorded in the [dedicated report](MACOS_EXIT_VALIDATION.md).
Original failures remain visible; neither the completed native checks nor later
successful Plugin repeats alone close the classification failure. The subsequently authorized
[test repair and acceptance](../module_system/MACOS_PLUGIN_CLASSIFICATION.md) add controlled
central-save ordering and failure cleanup evidence. The subsequent [example fixture repair](../../../examples/userExampleModule/README.md#macos-deployment-fixture-gap) passed focused Mac checks; historical full-suite failures remain recorded.

<a id="resource-coordinator"></a>

## Stage 1 implementation / 第一階段實作（2026-11-02）

`<vinc/osal/detail/resource_coordinator.hpp>` is the designated integration seam,
not a general Module API. `ResourceCoordinator` owns synchronization;
`ResourceCoordination` v1 copies only version/size, opaque context and acquire/release
entries. `ResourceGuard` owns one acquisition and releases through the same owner.
No synchronization object is freed by the borrowing image. Common remains unchanged.

`resource_coordination()` permanently selects this image's owner. Independent
standalone images get separate defaults; a default uses static process-lifetime
storage with intentionally suppressed destruction, so late static cleanup cannot
use an already destroyed owner. No explicit heap allocation, FD or worker is added.
An explicitly constructed owner may instead have a scoped lifetime, provided every
borrowing operation and unloading destructor finishes before it is destroyed.

`bind_resource_coordination()` validates before consuming the binding. Unsupported
versions, undersized tables and missing fields fail; a larger same-version table is
accepted with its unused tail ignored. After selection, replacement is rejected.
Publication uses an image-local atomic state; application access racing unfinished
binding returns Busy rather than using a different owner. The selected table is
immutable. There is deliberately no unbind/reset API.

Lock order within this seam is image selection (completed first), then the owner's
short bookkeeping mutex. The guard represents a non-recursive logical acquisition;
that bookkeeping mutex is released before caller resource work. `try_acquire()`
does not wait even for bookkeeping; `acquire_until()` waits on a condition variable,
honors cancellation before timeout, and accepts `Deadline::max()` for indefinite
waiting. Cancellation wakes waiters without a polling thread. Standard-library
exception boundaries remain unchanged. Exact ordering against resource/service
locks must be audited when those producers are wired in stage 2.

Plugin entry **11** passes coordination separately from fatal-only RuntimeServices.
The Host selects its default owner before loading, and `REGISTER_MODULE` binds the
Plugin copy before constructing the Module. Invalid fatal binding after coordination
does not revert the coordinator: initialization fails, the borrowed owner remains
valid for cleanup, and that Host ends. The macro's clear still withdraws only fatal
services; coordination stays bound through library destructors. Bootstrap **v16**
and RuntimeServices **v1** are unchanged because their formats did not change.

Once native loading succeeded, rejected libraries are retained by NativeSource.
The existing runtime attempts its bounded error report and shuts down Host workers;
source cleanup then uses `_Exit(70)` without unloading the rejected image or running
its destructors. This applies to invalid entries/versions and failed initialization:
constructors may have executed before validation, so quiescence is not assumed.
The central process still receives the failure/exit and continues. Normal accepted
Plugin cleanup retains paired destruction and orderly unload.

`vinc::module` now explicitly declares its OSAL dependency. Existing OSAL component
installation exports the updated archive/header, and the shared source/installed
Plugin macro carries entry 11; no additional runtime registry, target family or DLL
is needed. Clean installed-consumer and deployment acceptance is recorded in
[stage 4](#stage-4-acceptance).

**第一階段當時只建立協調與接線能力。** 該階段的 ResourceGuard 尚未接入原生 FD producers／spawn，
所以目前的過晚綁定測試透過新協調入口模擬初始化前使用；不能宣稱已攔下所有既有 OSAL
資源呼叫，第二階段才完成那些路徑。Windows 原生 HANDLE 行為未加鎖或改寫。
表格與借用只在同程序、相同工具鏈／組態／CRT 契約內使用；一般 Module 使用者不管理它。

### Stage 1 evidence / 第一階段驗證

Source: topic `codex/fd-race-validation`, base
`627c409423fd9bacb2de504a079da9f86120c0ad` plus this stage's working-tree changes.
No commit/push was requested. Local build/CTest logs are under ignored
`build/fd-coordinator/`.

| Environment | Evidence |
| --- | --- |
| Windows 11 x64, MSVC 19.44.35229 / toolset 14.44.35207, Release | Header smoke built; coordination + Module Plugin/initialization/Context/process/RuntimeServices **8/8** passed |
| Same Windows toolchain, standalone OSAL Debug | Common/OSAL **31/31** passed, including the three coordination suites |
| WSL Ubuntu 24.04, x86-64, GCC 13.3 / glibc 2.39, Release | Header smoke built; matching coordination/Module suites **8/8** passed |
| Same WSL toolchain, standalone OSAL Debug | Common/OSAL **34/34** passed, including the three coordination suites |

The two real test libraries link independent static OSAL copies. Tests prove shared
owner identity, exclusion across Host/two libraries, try/deadline/cancellation/wake,
moved-guard release, invalid/duplicate/late binding, and access from unloading-time
destructors. Production loader fixtures verify old entry rejection, failure isolation,
late-binding diagnostics across control transport, and rejected-image `_Exit(70)`
instead of its intentionally fatal unload destructor. Initialization tests verify
partial binding failure retains coordination and does not leak a fatal binding.

Windows/WSL evidence above covers that stage only. Subsequent native macOS execution
is recorded in the [Mac report](MACOS_FD_VALIDATION.md); existing baseline evidence
is preserved. Coordinator passes alone do not establish FD/stdio or exit-observation correctness.

After final wake-all/error-table/cause-chain refinements, the affected Release
checks passed **6/6** and Debug checks **4/4** on both Windows and WSL.
最後的喚醒、錯誤查詢與原因鏈調整，兩平台均重跑 Release 6/6、Debug 4/4 通過。

<a id="stage-2-implementation"></a>

## Stage 2 implementation / 第二階段實作（2026-11-02）

The POSIX producers now acquire the shared `ResourceGuard` before native FD
creation and retain it through relocation, required flags and ownership transfer
or rollback. `detail/posix/resource_guard.hpp` adds the current operation to a
coordination failure without losing its `AcquireResources` cause. Ordinary I/O
and shared-memory payload access do not acquire this guard.
Both forwarding helpers are image-local (`VINC_LOCAL`), like coordinator selection;
ELF interposition must not redirect them into another static OSAL image.

| Path | Protected work / ownership boundary |
| --- | --- |
| Process/Pipe/Socket monitors | epoll/eventfd or kqueue creation, relocation, flags, registration and local-owner cleanup |
| Linux process watch | pidfd creation, relocation, registration or close; existing watch-failure fallback is unchanged |
| Pipe pair | Native pair, both relocations, nonblocking/CLOEXEC setup and captured-writer blocking mode; local owners cover failures |
| Socket open/accept | Native endpoint, relocation, nonblocking/inheritance flags and local SocketState ownership |
| FileLock create/open | Open, relocation, identity/permission checks, then publish; failed create removes its new name, failed open never unlinks |
| SharedMemory create/open | shm_open, relocation, create sizing and publication; failed create closes and unlinks its own object under coordination |
| Notification | Separate directory and endpoint acquisitions; each returns a protected, isolated owner, without recursive acquisition |
| Explicit Pipe transfer | Duplicate the pinned endpoint above the bootstrap range; retry EINTR; never release the caller's endpoint on failure |
| Inherited stdio | Duplicate each requested valid stream under one guard, or record EBADF for child-only null-device setup |

Relocation helpers do not lock recursively. Pipe adoption receives already-owned
bootstrap FDs at or above 3 and creates no new FD; its existing validation and
ownership contract is unchanged. libc resolver and dynamic-loader internal FD
activity remain outside coverage.

### Ordering and waiting / 鎖順序與等待

ProcessService releases its state mutex before native launch preparation. Pipe
pair reservation similarly finishes before native creation, and publication into
service records happens after the native guard has been released. Explicit Pipe
handoff is the exception: `PipeState::mutex` pins endpoints before coordination.
Native acquisition helpers never take service/record/endpoint locks while holding
coordination, so there is no reverse coordinator → PipeState edge. No guard is
held across native spawn, child waiting/reaping, socket readiness, DNS or dispatch.

`try_accept()` uses only a try-acquisition and returns `WouldBlock` when busy.
`accept_until()` preserves the existing immediate attempt even at an expired
deadline; if coordination is busy it waits with the caller's deadline/token,
instead of polling an already-readable listener. POSIX SocketPoller creation
also uses its setup deadline/token for coordination. Cancellation and deadlines
do not interrupt native syscalls or promise hard real-time completion.

**沒有新增背景執行緒、傳輸路徑鎖或 Runtime registry。** 這是 native 資源建立與 spawn
準備的同步成本；原生呼叫仍可能受 OS／檔案系統延遲影響。Windows HANDLE 的建立與繼承
不加這把鎖，原有明確繼承清單與複製方式維持。

### Snapshot lifetime / 標準串流副本生命週期

`snapshot_stdio()` owns at most three CLOEXEC FDs. Their lower bound is
`3 + actual Pipe handoff count`, outside the complete destination range.
Missing streams become `/dev/null` only in child actions. Valid streams always
use source-to-destination dup2, not same-number dup2. The original parent flags,
presence and FD identities are not changed by spawn.

Coordination ends after the independent copies are owned; action construction and
native spawn use only those copies. Partial duplication/action failures close all
copies. After native spawn returns, copies are closed **before** pidfd acquisition,
so they do not unnecessarily consume the watch's descriptor budget. Native spawn
failure closes them as well. Error paths never fall back to original stdio numbers.

Three remains a fixed stream-count bound, not a setting. Copies preserve resource
identity/lifetime, while sharing the original open-file-description state and offsets.
Direct external stdio replacement during sampling is still outside the guarantee.

### Stage 2 evidence / 第二階段驗證

Source: `codex/fd-race-validation`, base `627c409423fd9bacb2de504a079da9f86120c0ad`
plus stages 1–2 working-tree changes. No commit/push was requested. Logs and
disposable reproducers are under ignored `build/fd-coordinator/` and
`build/fd-stage2-tools/`; the maintained regressions are in `tests/fd_spawn_tests.cpp`.

| Environment | Result |
| --- | --- |
| Windows 11 x64 build 26200, MSVC 19.44.35229 / toolset 14.44.35207 | Common/OSAL Debug **31/31**, Release **31/31** |
| WSL Ubuntu 24.04.5 x86-64, GCC/libstdc++ 13.3, glibc 2.39, CMake 3.28.3 | Common/OSAL Debug **42/42**, Release **42/42** |
| Native macOS | Subsequent Debug/Release Common/OSAL 42/42 and FD diagnostics are in the [Mac report](MACOS_FD_VALIDATION.md); exit stress still reproduces the separate observation defect. |

Final Linux image-local forwarding checks passed **11/11** in both Debug/Release;
`readelf` confirmed both forwarding symbols are HIDDEN. The final Linux Host/Plugin,
Context/process/RuntimeServices and coordination integration passed **8/8**, with
SDK header smoke built. Windows Release coordination/error/Plugin initialization
regression passed **6/6**; the native HANDLE implementation is unchanged.

Each Linux configuration additionally passed the following adaptation of the
original supplied harness. Instrumentation exists only in disposable source copies.
Scheduling first proves spawn cannot cross a paused native acquisition, then
releases relocation before waiting for spawn actions; it does not manufacture a
deadlock by requiring both operations to own the same guard.

| Suite per Linux configuration | Passed |
| --- | --- |
| Controlled resource acquisition / relocation / spawn schedules | **213/213** |
| Notification endpoint/keepalive schedules | **6/6** |
| Seeded acquisition race schedules | **100/100** |
| Uninstrumented stdio modes / missing-stream matrix | **144/144** |
| Uninstrumented CLOEXEC matrix | **144/144** |
| 1/63/64 Pipe handoffs with capture and missing stdio | **24/24** |
| Uninstrumented normal spawn/exit stress | **3,000/3,000** |

The maintained `osal.fd_spawn.*` regressions cover a **512-case** present/CLOEXEC/
Inherit-or-Null matrix, replacement immediately after snapshot acquisition,
native EMFILE at each of the three duplication positions, successful retry,
native spawn failure cleanup, producer participation, late binding after actual
Socket creation, ready-accept contention/deadline/cancellation, and **24** handoff
cases with inherited stdio alongside 1/63/64 endpoints. Assertions check bytes,
identity, flags, parent stream presence, EOF, exit status and resource reclamation.
Existing `osal.descriptor` separately checks FileLock owner exit while its child
remains alive, and create/open rollback with inherited descriptors.

Three isolated negative controls all failed as intended: use original stdio
numbers instead of snapshots; omit FileLock acquisition coordination; omit snapshot
close. Restoring each copied implementation passed. No production failure switch
or environment-variable fault hook was added.

Reproduce maintained tests on Linux/macOS with a compatible C++23 toolchain:

```sh
cmake -S src/vinc/osal -B build/osal-fd-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/osal-fd-debug
ctest --test-dir build/osal-fd-debug --output-on-failure
```

Repeat with a separate Release directory. Windows runs its native Common/OSAL
tests; POSIX-only tests are excluded by CMake. No Windows IOCP failure injection
is claimed by these results.

### Cleanup observation limit / 收尾觀察限制

An initial Linux Debug stress run had one immediate FD-set mismatch in 3,000
iterations; child bytes and exit were correct. That sample did not record the
extra FD identities or later cleanup, so its individual cause is not proven.
`ProcessService` already uses a detached observer: `worker_done` acknowledges
observation, not destruction of the worker's last shared-state reference.

A separate **test-copy-only** 50 ms delay after `run_monitor()` reproduced this
window **10/10** times: the remaining handles were epoll/eventfd, and all were
reclaimed after the final reference was released. This establishes a mechanism,
not a new production delay or proof of the original sample's exact cause.
The harness now records immediate and eventual FD sets, identities and cleanup
time, allowing a bounded 1-second test observation period after all public views
are dropped. The final Debug/Release stress passed without permanent FD growth.

此輪不改 `ProcessService` detached worker 或 shutdown／unload 契約。不能把
`shutdown_until()` 當作執行緒 join 或動態庫安全卸載屏障；需另行設計才可增加這種保證。
測試中的 1 秒只用來觀察清理，不是新 Runtime 參數，也沒有放寬等待子程序退出的期限。

**上述原始結論限定當時 Windows／Linux 的已測範圍。** 後續 macOS CLOEXEC 原生驗證及
退出觀察定位見 [Mac 報告](MACOS_FD_VALIDATION.md)；沒有加入輪詢或延長正式期限。
乾淨 installed SDK、搬移部署與平台驗收結果見[第四階段](#stage-4-acceptance)。

<a id="stage-4-acceptance"></a>

## Stage 4 integration acceptance / 第四階段整合驗收

### Scope and source / 範圍與來源

The 2026-11-03 acceptance uses `codex/fd-race-validation`, base
`627c409423fd9bacb2de504a079da9f86120c0ad` plus the uncommitted stages 1–2 repair
and stage 4 test/documentation changes. Windows and WSL use matching selected
first-party build/test inputs, checked by a 659-file SHA-256 manifest. Logs and disposable
builds are under ignored `build/fd-stage4/`; the manifest is
`source-manifest.json`. The source manifest excludes Markdown, JSON, image files
and dependency trees; pinned dependencies are reused. No commit or release is
implied by this record.

本輪驗收來源包含尚未提交的修復與測試接線；Windows／WSL 的第一方建置及測試輸入
已比對一致。使用 C++ App 設定，未啟用 Python Modules；這不是 Python binding
完整矩陣的重驗，也不改變既有 Python ABI／CRT 契約。

| Environment / 環境 | Toolchain / 工具鏈 |
| --- | --- |
| Windows 11 x64, build 26200 | MSVC 19.44.35229 / toolset 14.44.35207, CMake 4.3.2, Ninja 1.13.2; CPython 3.13.12 runs test/configuration tools |
| WSL Ubuntu 24.04.5 x86-64 | GCC/libstdc++ 13.3.0, glibc 2.39, CMake 3.28.3, Ninja 1.11.1; CPython 3.12.3 runs test/configuration tools |

Both platforms build UI resources with Node 24.19.0. Separate fresh Debug/Release
build directories enable tests and SDK installation. WSL builds on its native
Linux filesystem, not the Windows-mounted checkout. Native macOS was deferred
in that Windows/WSL run; the subsequent native acceptance below is separate evidence.

WSL core tests use its ordinary network environment. The remaining integration
suite runs as the same non-root `vinc-test` user inside a fresh Linux network
namespace with loopback enabled. This isolates its default UI port from concurrent
Windows acceptance and requires no production networking changes. Both suites
remain serial within their own platform.

### Results / 結果

| Platform / 平台 | Debug | Release |
| --- | --- | --- |
| Windows | **122/122 covered and passed** | **122/122 passed** |
| WSL Linux | **130/130 passed** | **130/130 passed** |
| Native macOS, subsequent `a19c0d4` acceptance | **132/132 passed** | **132/132 passed** |

Windows Debug combines 109 unaffected non-SDK successes, the corrected
`app.plugin` rerun and 12 SDK successes. The initial 109/110 run and the separate
passing rerun are both retained; this is not described as one uninterrupted
122-test run. Windows Release passes 110 non-SDK plus 12 SDK tests. Linux divides
the registered suite into disjoint core (89) and integration (41) groups. Nested
installed-consumer tests are included in their parent SDK result, not double-counted.

Raw logs: `windows-{Debug,Release}-{runtime,sdk}.log`,
`windows-{Debug,Release}-app-plugin.log`, and
`linux-{Debug,Release}-{core,integration}.log` under `build/fd-stage4/`.
The Windows initial clean-build command/exit/timing records are retained in
`windows-initial-build.json`; later incremental logs reuse the build-log names.
Windows and Linux process inspection after acceptance found no surviving
executable under this stage's build directories. The final Linux source check
still matched all 659 recorded inputs. These functional results do not establish
a performance bound or replace the deferred native Mac acceptance.

本輪 Windows／WSL 的來源建置、跨元件與安裝式 SDK 驗收完成。只補上測試接線與
驗收文件，沒有因該輪結果再變更正式 Runtime；當時 Mac 待驗的第二／四階段與
退出調查後續已由 [Mac 報告](MACOS_FD_VALIDATION.md)補上；第三階段修復及 Mac 後續測試
見 [目前驗收狀態](MACOS_EXIT_VALIDATION.md#current-acceptance-status)，Windows／Linux 修復後回歸仍待補。

### Maintained integration checks / 持續維護的整合測試

- `app.plugin` runs the existing rejection/lifecycle suite through the production
  App's Host dispatch. It rejects a fixture advertising old Plugin entry 10,
  missing exports, failed or throwing construction, initialization rejection
  and late coordinator binding;
  it also checks failure causes, Host exit and Plugin-owned destruction.
- `sdk.installed_consumer` copies the maintained OSAL coordination fixture into
  its external source directory and builds a consumer plus two independent
  libraries using only the moved SDK. Its nested four-test suite verifies the
  ordinary consumer, standalone coordination, shared ownership/exclusion with
  deadlines/cancellation, and rejection of late binding. These tests are not
  additional top-level CTest cases and their helpers are not installed SDK APIs.
- `sdk.installed_application` installs/moves an App, hides its SDK and consumer
  build directory, and runs from an unrelated cwd. Two distinct Host PIDs exchange
  custom nested messages, including copied/loaned publication and callbacks;
  Recorder persistence and UI HTTP preview are checked. Shutdown must report all
  Module exits. Package inventories reject a separately deployed ModuleHost or
  test executables. Schema edits, rejection/recovery and resource repair remain
  part of this fixture.
- Source App, Context, control, Bus, Param, RPC, fault/restart, Shell and system
  service regressions exercise startup isolation, repeated lifecycle operations
  and cleanup. UI checks use HTTP; no browser interaction is claimed.

本輪新測試接線重用元件既有 fixture，避免另寫一份協調器測試邏輯。安裝式測試
只使用搬移後 SDK 的標頭與函式庫，不把 producer 的 include 或 library 路徑補進來。

### Findings and remaining work / 發現與待驗事項

The first Windows Debug non-SDK pass found one test-wiring defect: `app.plugin`
still supplied seven fixture paths after `module_plugin_tests` began requiring
eight. It failed with `eight plugin fixture paths required` before exercising
the loader. Adding `plugin_late_coordinator` to the App test arguments corrected
the missing coverage. The dedicated Debug/Release reruns passed. This correction
does not change the production Runtime.

初次 Debug 驗收發現的是 App 測試少傳一個 fixture，不能當成已重現正式 Runtime
故障。保留初次失敗紀錄，另記修正後結果；沒有用略過案例來達成通過。

The subsequent native Mac round at `a19c0d4` passed stage 2 maintained tests and
Debug/Release source/App/Plugin and clean moved-SDK acceptance. Its source matches
repair commit `49cbfea` outside Markdown; the exact Mac inventory, configuration,
external controlled results and limitations are in the [Mac report](MACOS_FD_VALIDATION.md).
The 132-case inventory includes installed Python tests; do not assume identical
counts imply identical platform/configuration coverage. Separate unmodified exit
stress still failed 4/3000 (Debug) and 1/3000 (Release). Stage 3 now has traced
failure-mechanism evidence. Subsequent repair and controlled regressions are in [exit progress](PROCESS_EXIT_PROGRESS.md), with native/integration follow-up in the [step 3 report](MACOS_EXIT_VALIDATION.md); final acceptance remains open in the [root TODO](../../../TODO.md#osal-macos-退出事件後的回收進度).

### Reproduce / 重現方式

Use the same topic revision and compatible platform toolchain with the default
C++ App configuration. Initialize the pinned dependencies, then run:

```sh
cmake -S src -B build/fd-accept-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVINC_INSTALL_SDK=ON
cmake --build build/fd-accept-debug --parallel 2
ctest --test-dir build/fd-accept-debug --output-on-failure -j 1
```

Repeat in a separate `build/fd-accept-release` directory with
`-DCMAKE_BUILD_TYPE=Release`. On Windows, use the selected MSVC developer
environment; select a compatible `Python3_EXECUTABLE` for the configuration/test
tools if necessary. Close only authorized applications occupying the fixtures'
default UI port before acceptance. Do not run separate integration suites against
that same endpoint concurrently.

Mac 驗收須另記 OS／CPU／Clang／標準庫版本與實際測試結果。若再次出現既有的
退出觀察 timeout，保留失敗紀錄並對應第三階段調查；不要提高正式期限、略過失敗
或把 Windows／WSL 通過視為 Mac 通過。

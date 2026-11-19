# FD acquisition and spawn / FD 建立與程序啟動契約

POSIX resource creation and inherited standard-stream snapshots share a coordinator.
It protects OSAL-managed descriptor identity during launch preparation; it does not
serialize ordinary data transfer or intercept third-party native calls.
POSIX 共用協調器保護資源建立與啟動準備，不把日常收發加鎖，也不攔截任意第三方操作。
Windows retains native HANDLE duplication and an explicit inheritance list.

## FD acquisition and spawn / FD 建立與啟動契約

1. Coordinate the full native acquisition → relocation → required flag setup →
   publication or failure cleanup interval. Locking only the relocation helper is insufficient.
2. Coordination covers directly managed FD producers: Pipe, Socket/open/accept,
   FileLock/create/open, SharedMemory/create/open, Notification directories/endpoints,
   process/Pipe/Socket monitors, and pidfd, including failure and retry paths.
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
- Preserve lock ordering against service/record/endpoint locks. Helpers must not
  recursively acquire the same coordinator; ownership stays explicit.
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
The designated integration interface is documented in the [OSAL README](README.md).

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
7. Host, Plugin and SDK artifacts must have matching Plugin entry compatibility.
   Reject incompatible initialization explicitly; toolchain, standard-library,
   CRT and layout restrictions still apply.

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

## Exit observation and evidence / 退出觀察與證據

The macOS exit-observation repair is separate from FD creation. OSAL remains the sole
reaper; follow-up checks run only while a consumed exit notification still lacks a
result, without extending caller deadlines. See [exit progress](PROCESS_EXIT_PROGRESS.md).

- [FD 修復前證據](FD_RACE_VALIDATION.md)。
- [Windows／Linux 修復驗收](FD_SPAWN_VALIDATION.md)。
- [macOS FD 驗收](MACOS_FD_VALIDATION.md)、[退出驗收](MACOS_EXIT_VALIDATION.md)。
- [Windows／Linux 退出回歸](WINDOWS_LINUX_EXIT_VALIDATION.md)。

FileLock unlock failure ownership remains a separate outstanding issue in the README.

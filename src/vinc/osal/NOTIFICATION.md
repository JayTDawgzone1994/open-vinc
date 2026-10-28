# Cross-process notifications / 跨程序通知

`<vinc/osal/notification.hpp>`, target `vinc::osal`.

This general OSAL primitive has no Module, MID, Bus, or Context dependency. It
wakes a caller waiting for a hint; it does not dispatch callbacks or carry data.
The public entry points are `NotificationReceiver` and `NotificationSender`.
Both are move-only. The SDK umbrella also includes this header.

這是通用 OSAL 原語，不依賴 Module、MID、Bus 或 Context，只負責通知等待者，
不派送 callback、不傳資料。接收端與發送端均不可複製，可移動；SDK 整合標頭亦包含此 API。

## API and hints / API 與提示語意

| API | Contract / 契約 |
| --- | --- |
| `NotificationReceiver::create(name)` | Exclusively create an initially empty endpoint. Existing name → `AlreadyExists`, without clearing it. 獨占建立，既有名稱直接報錯，不清除既有通知。 |
| `NotificationReceiver::open(name)` | Open an existing endpoint for receiving; never create, reset, drain or unlink it. Missing name → `NotFound`. 開啟既有接收端，不建立、清空或刪除資源；不存在即報錯。 |
| `NotificationSender::open(name)` | Open an existing endpoint for notification only. 不提供讀取、等待或重設權限。 |
| `sender.notify()` | `Result<void>`; signal a hint without waiting for consumption. Concurrent calls supported. 不等待接收者處理，允許並行通知。 |
| `receiver.wait_until(deadline, stop)` | `Result<void>`; OS blocking wait, steady-clock deadline, `Timeout`/`Cancelled` errors. A second waiter on the same object returns `Busy`. 同一物件上的第二個等待者回 Busy；跨 handle 的單一等待者契約由呼叫端維持。 |
| `close()` | Explicit native release, idempotent after successful close. Not a cancellation mechanism. 關閉不是取消等待。 |
| `NotificationReceiver::unlink_name(name)` | POSIX owner removes discovery name; live endpoints remain usable. Windows returns `Unsupported`. POSIX 必須由指定擁有者清理名稱，析構不會自行刪除。 |

- Names are 1–24 ASCII letters, digits, `_`, or `-`; no path separators, dots,
  NUL, or encoding guesses. Invalid names return `InvalidArgument`.
- A hint sent before waiting remains available. Multiple hints may coalesce;
  extra wakes are allowed. Neither wake count nor `notify()` success counts messages.
- A pending hint may succeed even with an expired deadline. With no hint,
  an expired deadline provides a nonblocking check.
- Check actual work after waking. Work state and payload synchronization remain
  the caller's responsibility; this API is not a portable C++ memory fence for
  shared-memory payloads, nor a reliable message queue.
- There is no public `reset()`. A caller must not clear a hint after checking
  work and then sleep, which could erase a racing publication's only wake.
- Observed cancellation takes precedence. A cancellation may consume or leave
  a hint; a subsequent wait must tolerate an extra wake.
- `Deadline::max()` permits indefinite waiting, interruptible by the stop token.
  There is no timer polling, worker thread, shared pending-bit ownership, or
  application callback inside OSAL. OS scheduling still prevents hard latency bounds.

名稱限制為 1–24 個 ASCII 英數、底線或連字號。先通知、後等待仍能收到提示；通知可以合併，
也可能多醒一次，不能以喚醒次數推算訊息數。醒來後須重新檢查真正工作；共享記憶體的
可見性與一致性仍由資料協定負責。沒有公開 reset。取消優先於已觀察到的通知，但可能
消耗或留下提示。無限期限可用 stop token 取消；OSAL 不新增背景執行緒或定時輪詢。

## Ownership and failure / 所有權與失敗

Receiver handles may overlap for handoff. The creator retains its handle until
the recipient successfully opens the endpoint, then may close without affecting
the recipient. Pending hints are preserved by open. This is neither a receiver
ownership claim nor a broadcast API: callers must coordinate one active waiter
across all handles/processes. `Busy` checks only the particular C++ object's
wait state, not other opens. No cross-process lock or registry is added.

`open()` grants no additional name-cleanup authority. Only the designated resource
owner unlinks it. Opening a remaining POSIX FIFO does not prove the creator is
alive and does not restore data lost after all old descriptors closed. Lifetime
handoff requires overlapping live handles, not closing everything and reopening.
Windows opens a receiver with `SYNCHRONIZE | EVENT_MODIFY_STATE`, allowing both
wait and stop-token wake; sender rights stay unchanged. POSIX opens a separate
reader and keepalive writer, checking the same inode. Failure closes only the
new descriptors, leaving the existing endpoint and pending data untouched.
Both open roles use `ErrorOperation::OpenNotification`; no code numbers change.

交接時可重疊持有接收 handle；接手者成功 open 後，原建立者可關閉自己的 handle，
不影響接手者。open 不會清除既有提示。這不是獨占認領或廣播 API，跨物件／程序只有
一個等待者仍是呼叫端的契約；Busy 僅檢查同一 C++ 物件，不新增跨程序鎖或登記服務。
open 不授予刪除名稱的權利，亦不證明建立者仍存活。POSIX 留存的 FIFO 可以被開啟，
但全部舊 FD 關閉後遺失的提示不會因此恢復，因此交接須維持 handle 存活重疊。
開啟中途失敗只關閉本次取得的資源，不能刪除或清空既有端點。

Creation/opening may allocate; existing STL exception policy remains. Notify and
native wait use pre-established resources and fixed-size local storage. All
expected failures use `vinc::Result` with OSAL codes and operations; native failures
retain Win32/errno values. New operation numbers are 49–54, without renumbering old values.

`close()`, move, assignment, and destruction must not race any operation on that
object. Keep the receiver alive until both the wait and stop callback finish.
Cancel, join the waiting caller, then close. Destructor cleanup is best effort;
use explicit `close()` when release errors must be observed. POSIX never retries
`close(EINTR)` or reuses that descriptor value; Windows retains a failed-to-close
handle for an explicit retry.

Publish a name only after successful creation. The caller distributes names and
assigns generation identities. Never reuse an old generation's name until all
senders and delayed cleanup are gone. Unlink is not a broadcast cancellation.
OSAL does not scan stale resources or infer process death from a PID.

**Successful notification does not prove that the receiver is alive or handled
anything.** POSIX can report `PeerClosed` when no read endpoint remains; Windows
senders retain the Event even after the receiver exits. Unexpected OS wait/wake
failures are errors, not a promise that cancellation can overcome an arbitrary
kernel/resource failure. Neither backend promises wait-free execution of OS calls.

建立／開啟可能配置記憶體，保留既有 STL 例外政策；預期失敗使用統一 Result。
關閉、移動、析構不可與使用並行。正確順序是取消、等待呼叫端離開，再關閉。
需檢查釋放錯誤時明確呼叫 close；析構只有 best effort。

名稱由呼叫端分配與傳遞；成功建立後才能公布。同一世代的舊發送者與延遲清理完全結束前，
不得重用名稱。OSAL 不推斷程序存活、不自動掃描或刪除殘留資源。
通知成功不是存活或送達確認；Windows 的發送 handle 仍會保留 Event，POSIX 則可回報無讀者。
不承諾任意原生資源故障下仍能成功喚醒，也不承諾 OS 呼叫具有 wait-free 或硬即時上限。

## Native backends / 原生後端

| Platform | Mechanism / 機制 | Namespace and cleanup / 名稱與清理 |
| --- | --- | --- |
| Windows | Named auto-reset Event; `SetEvent` / `WaitForSingleObject`. Private current-user DACL; non-inheritable handles. | `Local\ov_notify_<name>`, current Windows session. OS removes it after the last handle closes. |
| Linux | FIFO with independent nonblocking read and keepalive-write descriptors; `poll` / bounded read. Thread-local SIGPIPE masking/draining preserves the previous mask, disposition, and existing pending signal. | `/tmp/ov-osal-notify-<effective-uid>/<name>`; owner unlinks. |
| macOS | Same FIFO/readiness approach; per-descriptor `F_SETNOSIGPIPE`, without a process-global handler. | Same effective-user namespace; owner unlinks. Native validation of this new primitive is pending. |

POSIX creates the directory with mode `0700` and FIFO with `0600` (subject to the
caller's umask). It rejects symlinks, foreign ownership and group/other access,
and verifies that opened endpoints still match the inspected device/inode.
Descriptors use `CLOEXEC` and are isolated from standard streams. A failed create
closes acquired descriptors and attempts to unlink only its newly created name;
a failed open never removes someone else's resource. The empty private directory
is retained to avoid namespace races. Malicious actions by another same-user
process are outside this cooperative IPC contract; these permissions are not a sandbox.

A full FIFO already contains hints, so nonblocking `EAGAIN` counts as successful
coalescing. Each wait reads at most 256 token bytes once, then returns; a stream
of producers cannot trap the receiver in an unlimited drain loop. Windows may
coalesce to one signal. The backend token count/capacity is not a portable API.

POSIX 使用受限權限目錄與 FIFO，檢查連結、擁有者、權限及 inode；FD 不可意外繼承，
也不占用原本缺少的標準輸入輸出。建立失敗只回滾自己新建的名稱，開啟失敗不刪他人資源。
FIFO 滿載時已有提示，視為通知合併；每次最多讀取 256 bytes 後返回，不會無限清理。
Windows 只保留合併後的 Event 狀態，兩平台不保證相同通知數。這不是對同帳號惡意程序的沙箱。

## Validation / 驗證

2026-10-28, Windows 10.0.26200 x64, MSVC 19.51.36257, Windows SDK 10.0.26100.0,
CMake 4.3.2, Ninja 1.13.2; WSL2 Ubuntu 24.04.5 x86_64, GCC/libstdc++ 13.3.0,
glibc 2.39, CMake 3.28.3, Ninja 1.11.1.

`osal.notification` covers names/errors/static catalog, exclusive creation without
clearing a hint, notify-before-wait, deadlines, moves/close, cancellation of an
indefinite wait, Busy admission, 80,000 notifications without a consumer, multiple
producer threads/processes, actual process exchange and killed-receiver behavior,
recreation after cleanup, and repeated lifecycle cleanup. Windows additionally
checks handle counts and actual Event creation/open failure against a native
object of the wrong type.

Receiver-open coverage additionally verifies invalid/missing names, preserved
pre-open hints, local and actual child-process handoff, continued receiving after
creator close, cancellation on an opened receiver, move/close, and native resource
counts. POSIX injects receiver-open FD exhaustion at both endpoint stages and
checks unchanged FIFO identity/data with no descriptor leak; symlink and stdio
isolation probes include receiver open. This tests the OSAL API, not ModuleHost wiring.

接收端 open 補測非法／不存在名稱、保留先到提示、同程序及真正子程序交接、建立者關閉後
仍可接收、取消、移動、關閉與資源計數。POSIX 在讀端／保活寫端兩階段注入 FD 耗盡，
確認 FIFO 身分、資料及 FD 計數保持正確；亦涵蓋符號連結與標準串流隔離。
這些是 OSAL 測試，並未接上 ModuleHost。

`osal.notification_native` on POSIX covers FD exhaustion at both receiver endpoint
acquisition stages, failed sender open without deleting the receiver, SIGPIPE
default/custom/ignored/pre-existing-pending policies, descriptor counts and
`CLOEXEC`, missing stdio, private FIFO permissions, symlink rejection, and EINTR
without premature timeout. Limit fixtures preserve inherited descriptors and do
not require fd 3 to be unused. CTest bounds each executable to 45 seconds.

測試涵蓋通知競爭、取消、滿載、真正跨程序交換與接收者死亡、重建與資源回收；POSIX 額外
注入 FD 耗盡及訊號條件。Windows 額外驗證原生物件型別衝突失敗及 handle 數量。
FD fixture 不假設 fd 3 空閒，不會更動啟動測試者的限制或訊號設定。

See the current run results in the [OSAL README](README.md#notification-validation).
The new macOS notification backend has not been built/run on macOS in this round.
This does not invalidate prior macOS validation of other OSAL facilities.

## Follow-up / 待辦

- Run both new tests and OSAL regression tests on native macOS, especially FIFO
  `F_SETNOSIGPIPE`, cancellation, resource exhaustion and namespace cleanup.
- OS-native failures such as Windows kernel allocation exhaustion and cancellation
  wake syscall failure have not been injected. Do not describe coverage as exhaustive.
- Name cleanup is best effort during rollback; filesystem failures can leave a
  newly created FIFO. Owners must diagnose failed cleanup, not blindly reuse names.
- Bus/ModuleHost/Context integration is a separate phase in the
  [pump wakeup design](../module_system/PUMP_WAKEUP.md). No pump latency improvement
  has been measured or claimed for this OSAL-only change.

待補 macOS 新通知功能的原生驗證。未逐一注入 Windows 核心資源不足或取消喚醒 syscall
失敗，不宣稱完整故障覆蓋。檔案系統失敗可能讓回滾留下 FIFO，擁有者不可盲目重用名稱。
Bus、ModuleHost 與 Context 尚未接線，這輪沒有宣稱已消除 pump 延遲。

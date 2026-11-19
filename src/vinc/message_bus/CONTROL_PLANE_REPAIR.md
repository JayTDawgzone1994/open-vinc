# Message Bus control plane / Message Bus 控制面契約

BusManager owns App-wide registrations. Each Host synchronizes only the endpoints
needed by its own publications/subscriptions; Module code still subscribes by Topic.
The UI obtains the full graph separately through central diagnostics.

中央知道全 App 的關係；ModuleHost 只同步自己收發用得到的端點。這裡的「拓樸」就是
Publisher／Subscriber 的連接關係，不要求使用者自己維護。

## Slow Hosts and bounded synchronization / 落後與有界同步

- A healthy but lagging Host keeps its Session and catches up. Lag alone never
  triggers a restart; heartbeat loss, malformed protocol and OS exit retain their
  own policies. Diagnostics expose applied/target versions and last progress.
- Send versioned changes in bounded pages, sharing Publisher descriptions while
  preserving each subscription's ID, reader index and initial floor.
- Keep one page in flight per Host. Backpressure retries without new registrations
  being required, without blocking the central worker and without replaying Registry operations.
- When retained changes exceed their budget, switch to a consistent snapshot for
  that Host. It contains no unrelated endpoints. Apply the complete version atomically;
  never expose a half snapshot. Subsequent changes follow it.
- Bind pages/receipts to the Host generation and transfer. Duplicates cannot apply
  twice; missing/base-mismatched data resynchronizes. Reject malformed frames.
- Bound history, prepared pages and Host staging separately from live registration
  storage. Slow Hosts cannot retain unbounded historical versions.

心跳正常但落後就繼續補。變更停止或處理速度足夠時，即使沒有新操作也能收斂。
暫存預算、格式與 ACK 規則統一由 [Bus design](DESIGN.md) 維護，不在本頁複製協定表。

## Visible endpoint behavior / 端點語意

Existing sources keep their cursors; newly registered sources use their original
floor. Delayed pages cannot revive a locally closed subscription. Callback and
get-last readers keep ordering, copy protection and loss accounting.

A missing mapping is checked against central state in the background. While that
check is pending, pump/get-last can continue other sources and report no data when
appropriate. Confirmed retirement removes the source; a still-registered source
that cannot reopen reports its original error. Do not block a Topic waiting for data.

`close()` succeeds after central registration/empty-Topic cleanup, not after every
observer applies the topology. A topology receipt never releases Reader protection.
See [endpoint release](DESIGN.md#endpoint-release).

## Deadlines and resource reservations / 期限與預留

| Boundary / 邊界 | Contract / 契約 |
| --- | --- |
| Waiting for an earlier release ACK | Share one deadline with the new call. Recheck after ACK and before allocating the new request ID/pending state. If expired, send nothing, return Timeout and leave the Session usable. |
| Request already accepted by transport | A lost reply may leave the result unknown. Do not automatically replay a mutation or claim it never executed. Read-only queries retain their own policy. |
| Reserved counter/control/payload name | Exactly one owner handles each reservation from acquisition through transfer or cleanup. |
| Create/rollback failed | Retire only the reservation when OSAL confirms no owned name needs cleanup. Uncertain cleanup remains journaled for restart recovery; preserve the primary error and separate cleanup diagnostics. |

同名衝突不能刪掉他方物件；App 重啟清理的保留命名空間政策由
[Instance](../module_system/INSTANCE.md) 定義。完整建立結果見
[Topic recovery](TOPIC_REGISTRATION_RECOVERY.md) 與 [OSAL](../osal/SHARED_MEMORY.md)。

## Validation / 驗證

Bus protocol is v6; current Plugin compatibility follows [Module System](../module_system/README.md).
[Windows/WSL regression and costs](CONTROL_PLANE_VALIDATION.md) preserve pre-fix
failures and successful reruns. [macOS acceptance](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)
covers native overload, recovery, clean SDK and costs. These are scoped results,
not a guarantee of delivery under unbounded load.

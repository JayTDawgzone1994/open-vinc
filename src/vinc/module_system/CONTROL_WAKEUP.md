# Central control wakeup / 中央控制接收喚醒契約

The single central supervisor wakes when Control Channel has relevant work.
It keeps bounded fair passes and the existing 20 ms supervision fallback.

中央收件不再只等固定補查；通知讓既有 supervisor 提早檢查。沒有增加每 Session 的執行緒，
也沒有改變 Context pump 或共享記憶體資料通知。元件 API 見 [Control Channel](../control_channel/README.md#internal-receive-wake--內部接收通知原語)。

## Ownership / 責任

`control::detail::ReceiveWake` holds a coalesced process-local hint. Sessions keep
lifetime-safe associations; messages remain in the original bounded queues.
Notifications cover queued application frames, handshake completion, termination,
and work already present at binding. Ordinary heartbeats do not wake it per frame.
Unbound Sessions continue using their normal receive API.

I/O 執行緒只更新提示，不執行 Manager、Bus 或使用者 callback。通知不表示操作成功、
程序已退出或共享資源可以回收，也不為每筆訊息配置另一份 queue／OS handle。

## Waiting and fairness / 等待與公平性

| Situation / 情況 | Behavior / 行為 |
| --- | --- |
| Ready work | Process at most 16 frames per Module per pass, preserving Session FIFO. 每輪每 Module 最多 16 幀。 |
| Budget exhausted | Let the other Modules run, then continue without forgetting backlog. 其他 Module 先完成本輪，再繼續。 |
| No ready work | Wait for receive, management, stop or the next supervision deadline. |
| RPC downstream full | Keep the original request and FIFO order; retry at the 20 ms fallback. Do not self-wake continuously or replay accepted work. |

Pending state and the wait predicate share a dedicated mutex. Consume existing
hints before scanning; hints arriving during scanning or before sleep survive
for the next pass. Binding/backlog handoff, rebinding and stop use the same reliable
handoff. A bare notify without remembered state is insufficient.

健康補查使用絕對期限，不被持續流量反覆延後。滿載 RPC 的「等待容量」與可立即處理的
收件分開，不因 queue 非空就忙迴圈。20 ms 是政策間隔，不是硬即時保證。

## Locks and shutdown / 鎖與停止

- Never hold the wait mutex across Record scans, dispatch, user hooks or joins.
  I/O notification must not acquire Manager registry/record locks in reverse order.
- Preserve Engine → Peer ordering. Safe unbinding and shared lifetime prevent late
  notifications from touching destroyed or replacement records.
- Stop wakes waiters. Retain notification state until waiters/in-flight references
  finish; never join under locks needed by those users.
- Preserve the first primary fault, separate cleanup diagnostics and existing
  deadlines/restart policy. Only confirmed OS exit permits shared-resource recovery.

Subscribe 與 EnableNotifications 仍是兩個請求；公開 API、wire 與 payload 不因喚醒而改版。
Host／Plugin／SDK 仍須使用相容建置，不能從「wire 沒變」推論任意二進位相容。

## Validation and limits / 驗證與限制

- [Controlled wake, lifecycle and saturation tests](CONTROL_WAKEUP_TESTS.md).
- [Windows/WSL integration, corrected evidence and costs](CONTROL_WAKEUP_VALIDATION.md).
- [macOS source, SDK and costs](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md).

外部應用原有 491 筆／30 秒 Ready 條件仍需其自身整合驗收；框架測試不能替代。
不以縮短輪詢、放寬期限或減少訂閱數作為本契約的修復。

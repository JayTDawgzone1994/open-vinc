# Host shutdown / Host 收尾契約

Finish user cleanup while supervision is alive; stop and join the receiver before
retiring the services it can access. Module lifecycle details remain in the
[Module System README](README.md).
先保留監督完成使用者收尾，再停止並等候接收者退出，最後銷毀它會存取的服務。

## Shutdown contract / 收尾契約

1. **Receiver lifetime precedes resource retirement.** Complete callback drain,
   on_stop, Module/callback destruction and required completion/failure exchanges
   under the existing lifecycle policy. At final teardown, publish finished and
   request watchdog stop, join the watchdog, then shut down Bus and close the
   lifecycle Session. Retain adapters and Plugin code for their existing required
   lifetimes. Never join while holding a mutex needed by the watchdog.
2. **Preserve supervision.** Do not stop the watchdog before user cleanup and
   required exchanges finish. Keep central supervision, original stop deadlines,
   failure precedence and restart policy. Do not restart a deadline to accommodate
   join. Reuse the existing bounded receive poll (20 ms default); it is not a hard
   wall-time guarantee or a bound on arbitrary in-flight work.
3. **Preserve real failures.** Do not treat all errors during stopping as success,
   and do not rely on a second unsynchronized finished check. An already-closed
   Bus receive reports the existing Closed classification; malformed protocol
   remains Protocol. Keep appropriate layer wrapping and cause chains. Callers inspect this classification without losing lower-layer causes.
4. **Keep first-generation test evidence.** Normal-completion tests must expose an
   unexpected exit without a successful retry obscuring it. Check the retained
   generation, primary/cleanup errors and actual exit before querying an obsolete
   MID. Dedicated restart tests keep their explicit restart budgets.
5. **Cancellation is not destruction.** Notification, Param, RPC, locks, queries
   and Log retain their adapters while in-flight operations finish. Stop must
   still wake or cancel blocked users; joining precedes destruction of services
   those users can access.

最終順序：完成原有使用者收尾／協定 → finished＋request_stop → join watchdog →
關閉 Bus／Session → 依既有所有權銷毀服務、最後卸載 Plugin。
不新增背景執行緒、公開 API、ABI、共享記憶體布局或可調參數；不改 pump worker
數量、正式重啟政策，也不以加長 timeout 或忽略錯誤作為修復。

## Evidence / 證據

The [investigation](CONTEXT_SHUTDOWN_INVESTIGATION.md) preserves pre-fix failures,
controlled causal evidence and limitations. Controlled reproduction does not establish
missing facts about every original intermittent failure.

- [Windows／WSL regression](CONTEXT_SHUTDOWN_VALIDATION.md)。
- [macOS C++ Host／Context](MACOS_CONTEXT_SHUTDOWN_VALIDATION.md)。
- [macOS 最新來源 Python、乾淨 SDK 與搬移](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)。

The implementation does not relax timeouts or treat arbitrary stopping errors as success.

# Topic registration recovery / Topic 註冊失敗回收契約

Central records a reserved resource name before native creation so a crash can be
recovered on the next same-name App startup. Failure must not leave a reservation
when OSAL can confirm that no owned object remains to clean up.

中央先預留，再建立原生物件；失敗時依「這次是否留下自有物件」決定是否撤銷，
不能只看錯誤碼。Subscriber-only Topic 也會建立共享計數器，並非零 OS 資源成本。

## Ownership / 所有權

| Result / 結果 | Action / 動作 |
| --- | --- |
| Reservation itself failed | Return the reservation error; retire nothing not acquired. |
| OSAL confirms no new owned object | Retire this reservation only; preserve any pre-existing same-name object. |
| New object created, initialization failed, cleanup confirmed | Retire once; return the original initialization error. |
| Native cleanup or journal retirement uncertain | Keep recovery evidence and cleanup diagnostics. Never report successful cleanup. |
| Object delivered successfully | Transfer cleanup to its normal owner exactly once. |

OSAL reports native ownership/rollback state; Bus applies that result; Instance
owns the journal. A generic unlink helper is not a substitute for retiring a
reservation. Partial creation, mapping failure and exception unwinding follow
the same ownership rule. Windows closes only handles acquired by this attempt;
POSIX verifies unlink results after a successful exclusive create.

`BusManager::last_registration_cleanup_failure()` preserves the primary registration
failure and affected names/cleanup errors. The caller retains the original error
chain, while uncertain names remain available for restart recovery.

## Isolation and recovery / 隔離與恢復

Repeated rejected registrations must not grow the journal when cleanup is confirmed.
Existing Topics continue communicating; freeing capacity permits a new registration.
Two Apps may use the same Topic name: restarting A cleans A's old generation,
while B's resources and existing endpoints remain usable.

框架保留名稱的邊界與同名 App 啟動清理見 [Instance](../module_system/INSTANCE.md#建立前記錄啟動時清理)。
不宣稱能防止同帳號外部程式繞過 Context、直接搶占內部原生名稱。
Topic 數量上限與 OS FD／handle／記憶體限制是不同限制，見 [Message Bus](README.md)。

## Validation / 驗證

[Windows/WSL evidence](TOPIC_REGISTRATION_VALIDATION.md) includes real EMFILE,
native name collisions, injected uncertain cleanup and two-App crash/restart.
[macOS evidence](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md) adds native FD/recovery
and repeated isolation checks. Linux linker-only fault injection remains Linux-only.

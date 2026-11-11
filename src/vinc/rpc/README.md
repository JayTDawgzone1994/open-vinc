<a id="rpc"></a>

# RPC / 具型別請求／回覆

**Stages 1–4 implemented and accepted on Windows/WSL:** the [change contract](CLIENT_CONTRACT.md) defines the
synchronous `Context::call_rpc()` and reusable asynchronous `Context::rpc_client()`.
Both C++ and Python use `module.service` addresses. Old per-call async entry points
and split-target public syntax have been removed. Clean installed-SDK acceptance
is recorded below, including the initial Windows Release UI failure and successful
retry; native macOS source and fresh SDK-only acceptance are linked below.

**第一至四階段已完成 Windows／WSL 驗收：** C++／Python 均提供同步 call_rpc 與可重複非同步 RpcClient，
使用 `module.service`。舊逐次傳入 callback 的公開入口已移除；乾淨 SDK 結果、
Release 首輪 UI 失敗與成功重跑均保留於下方。原生 macOS 來源及乾淨 SDK-only 已補驗，見下方後續紀錄。

Current Host/Plugin ABI follows [Module System](../module_system/README.md).
The subsequent entry-17 Manual-wait change is validated separately in
[Pump wakeup](../module_system/PUMP_WAKEUP.md#manual-wait-api-2026-11-09--manual-等待介面);
the entry-16 results below retain their original scope.
目前 ABI 以 Module System 為準；後續 entry 17 的 Manual 等待另有驗收，
下方 entry 16 的歷史結果不改寫成新版測試證據。

<a id="integration-acceptance"></a>

## Integration acceptance (stage 4) / 整合驗收（第四階段）

2026-11-06, same working tree, Plugin entry 16 / bootstrap v16 / RPC wire v1.
No production Runtime behavior changed in this stage. The installed Python fixture
now builds a second native Manual Module, `RpcProbe`, against the installed SDK and
its own generated `SdkPacket` schema. Both languages perform synchronous RPC before
the caller's first pump. The native probe also checks Busy, callback resubmission,
pump blocking rejection and independent retained responses. The relocated App runs
with the SDK and consumer build hidden, then exits through normal central shutdown.
Schema revision changes repeat the cross-language checks with regenerated messages.

本輪未改正式 Runtime；新增安裝式 SDK 的 C++ Manual probe，以自訂生成訊息測試
C++→Python 與 Python→C++ 同步呼叫、不依賴 caller pump、非同步重用與 pump 防護。
整包搬移、隱藏 SDK／consumer build 後實際執行，亦在訊息 schema 更新後再次驗證。

| Environment / 環境 | Source regression / 來源回歸 | Fresh installed SDK / 乾淨安裝驗收 |
| --- | --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja, CPython 3.13.12, Debug | 13/13 RPC + Module tests, 60.08 s | All three suites passed: consumer, Python and application / 三套全數通過 |
| Same Windows toolchain, Release | Same 13/13, 54.90 s | All three suites passed; application required a retry (see below) / 三套通過，App 曾重跑 |
| WSL2 Ubuntu 24.04 x64, GCC/libstdc++ 13.3.0, CMake 3.28.3, Ninja, CPython 3.12.3, Release | All ten `rpc.*` plus `app.rpc`: 11/11, 10.35 s | All three suites passed: consumer, Python and application / 三套全數通過 |
| Native macOS | Not run in this stage | Not run; root TODO retains required acceptance |

The fresh SDK producers use `VINC_INSTALL_SDK=ON`, `VINC_BUILD_APPS=OFF`,
`BUILD_TESTING=OFF`; installed consumers are separate CMake projects. They execute
`installed_consumer.cmake`, `installed_python.cmake`, and `installed_application.cmake`
from `src/vinc/sdk/tests`, with matching CONFIG/compiler/generator/Python arguments.
The SDK suites cover header/linking, relocation, generated messages, Bus/Param/RPC,
Python ABI/metadata failure isolation, incremental schema edits, resource repair,
managed-file removal and normal shutdown. They are not the full project test suite.
Windows uses its existing static Python runtime closure; no loading policy was relaxed.

乾淨 producer 不建置 App／tests；另外以 find_package 消費安裝後的 SDK。
沿用三份維護中的驗收腳本，並非全專案 CTest。Windows 保持既有 Python 靜態 Runtime 規則。

Reproduction uses a fresh producer directory and an initialized compiler environment:

```sh
cmake -S src -B <producer> -G Ninja -DCMAKE_BUILD_TYPE=<Debug-or-Release> -DVINC_INSTALL_SDK=ON -DVINC_BUILD_APPS=OFF -DBUILD_TESTING=OFF
cmake --build <producer> --parallel 4
cmake -DSDK_BUILD=<producer> -DWORK=<producer>/sdk-install-test -DFIXTURE=<repo>/src/vinc/sdk/tests/<suite> -DCOMPILER=<compiler> -DCONFIG=<Debug-or-Release> -DMAKE_PROGRAM=<ninja> -DGENERATOR=Ninja -DPYTHON=<python> -DWITH_UI=ON -P <repo>/src/vinc/sdk/tests/<suite>.cmake
```

Run the last command for `installed_consumer`, `installed_python` and
`installed_application`; quote path arguments containing spaces. Select matching
compiler/Python paths for the producer as recorded above. The fixtures own their
named work directories under the producer and recreate those directories on each run.

Coverage map for the [contract checklist](CLIENT_CONTRACT.md#acceptance-checklist--驗收條件):

| Contract | Evidence |
| --- | --- |
| Addresses, owning messages, Busy, options, move/close/destruction and revoked endpoint | `rpc.client_foundations` |
| Application status, schema/capacity failures, large images, async self-call, fixed pump batch | `rpc.process`, `rpc.contracts`, `rpc.python` |
| No-pump calls, concurrency, caller-thread pump guard, response preservation | `rpc.synchronous`, installed `RpcProbe` and `PythonPeer` |
| Timeout, late/duplicate events, shared budgets, stop wakeup, closed-channel admission | `rpc.synchronous_transport`; closed-channel failure also returns reserved capacity |
| Actual target death and callback exception isolation | `rpc.target_exit`, `rpc.synchronous_target_exit`, `rpc.handler_failure`, `rpc.python` |
| Lease cleanup and same-name restart identity | `rpc.recovery`: old/new service and token differ; old owner receives Protocol for the new token; retired events cannot release the replacement lease |
| GIL release, owned replies, callback resubmission and local exception policy | Stage-3 `rpc.python` plus installed cross-language flows |
| Public headers, generated external schemas, relocation and shutdown | SDK header smoke from stage 3; fresh installed suites above |

Restart identity is a controlled Manager transition test, combined with separate real
process-death tests; it is not a new end-to-end auto-restart stress run. Lifecycle checks
cover the documented serialized handle contract, revocation and framework wait/stop
paths. They do not promise arbitrary concurrent manipulation of user handles/captures,
general deadlock freedom, or a returned Result before central-loss forced termination.
Heartbeat/user-callback ownership remains as defined in Module System.

重啟身分以受控 Manager 測試配合真實程序死亡案例驗證，不宣稱另跑了自動重啟壓力測試。
生命週期驗收不擴大為任意使用者 handle／capture 的並行安全保證；中央失聯仍遵守終止契約。

**Retained initial failure:** Windows Release `installed_application.cmake` first
failed in the plain-App HTTP flow with UI BindFailed / HTTP 401. Bus/Recorder and
normal process shutdown succeeded. Port 31411 had a listener (PID 24176), but it
exited before its process identity was inspected; ownership remains unconfirmed.
After the listener disappeared, a complete unchanged application-suite rerun passed
in 269.73 s. This is not evidence of an RPC regression, nor proof of the listener's
origin. Fixed-port test isolation remains a separate [SDK concern](../sdk/README.md#待處理事項).
Original logs are retained under `build/rpc-stage4-sdk-release/first-application-failure/`;
the successful run is `installed_application-retry.log`. Windows Debug application
passed on its first run (274.65 s); Python suites passed Debug 626.09 s / Release 638.41 s.

**保留首輪失敗：**Release App 曾發生 UI 綁定失敗／401；未確認 listener 歸屬。
埠釋放後原設定完整重跑通過，未修改或放寬 Runtime。固定埠測試隔離另列 SDK 待辦，
不把這次重跑描述成首輪全數通過，也不宣稱已查明占用程序來源。

Local evidence (ignored build artifacts, not repository inputs):
`build/rpc-stage4-{windows-debug,windows-release,final-debug,final-release,recovery-debug,recovery-release}.log`,
`build/rpc-stage4-sdk-{debug,release}/` suite logs and their `sdk-python-test/flow-*.log`.
WSL equivalents are under `/home/vinc-test/python-module-deploy-20261105/`, including
`rpc-stage4-final.log`, `rpc-stage4-recovery.log`, and `rpc-stage4-sdk-release/`.
The final stronger Protocol assertion in recovery is separately rebuilt/retested on
both Windows configurations and WSL. Prior source evidence remains explicitly scoped
below; macOS and minimum-MSVC 19.44 are not established by these runs.

<a id="async-clients"></a>

## Reusable clients and Python (stage 3) / 非同步 Client 與 Python（第三階段）

Context now issues `RpcClient<Request, Response>` through a weak, revocable endpoint.
The detail AsyncCall state machine is shared with Python, so Busy, snapshot admission,
callback resubmission, token validation and close behavior have one implementation.
The endpoint serializes only admission against destruction; it never holds its mutex
through reply waiting or callback execution. Host remains the sole transport owner.
Plugin entry **16** reflects the added Context-owned endpoint; rebuild connected Plugins,
Host and Python bindings. Wire v1 and bootstrap v16 remain unchanged.

Context 已接上可撤銷的非同步 Client；C++／Python 共用狀態機，不另建服務或執行緒。
Plugin entry 16 要整套重建；wire／bootstrap 未改。公開 API、Python 所有權與 GIL 契約見下方。

Validation on 2026-11-06 (incremental source builds):

| Environment | Results |
| --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Debug | Nine `rpc.*` + `module.context/plugin/manager_faults/runtime_faults`: **13/13**, 48.88 s. SDK public-header smoke and `vinc_rpc_example` built. |
| Same Windows toolchain, Release | Same **13/13**, 47.07 s; SDK public-header smoke built. |
| Windows Debug, CPython 3.13.12, pybind11 3.1.0 | `rpc.python`, `module.python`, `module.python_services`, `module.python_messages`: **4/4**, 27.49 s. Isolated source multi-App fixture, static Python runtime chain. |
| WSL2 Ubuntu 24.04 x64, GCC/libstdc++ 13.3.0, CMake 3.28.3, CPython 3.12.3, Release | Ten `rpc.*`, four Module regressions, Python lifecycle/services: **16/16**, 29.66 s; SDK public-header smoke built. |

Added coverage includes real Plugin callback-driven reuse and snapshot ownership, Busy
before dispatch, close suppression with eventual transport cleanup, asynchronous self-call,
and a retained real endpoint after its native owner is destroyed. Python verifies a no-pump
synchronous reply, another Python thread progressing during the GIL-released native wait,
old-response preservation, synchronous pump/self rejection, Busy, callback resubmission,
close during dispatch, timeout delivery, and an escaping RPC callback exception with
preserved diagnostics while central and the C++ peer continue. Existing target-exit,
lease recovery, bounded wait, lifecycle and service tests also ran.

新增真實 Plugin／Python 行為驗證；Python 故意拋出 callback 例外後，確認該程序退出、
診斷保留、中央與 C++ peer 仍可工作。生命週期驗證不宣稱一般使用者程式無資料競爭或死鎖。

Commands: build the stage-2 targets below plus `rpc_python_tests`, `module_python_tests`,
`module_python_services_tests` where Python is enabled. Run all `rpc.*` and
`module.context/plugin/manager_faults/runtime_faults`; also run
`module.python`, `module.python_services`, and (Windows here) `module.python_messages`.
Windows logs: `build/rpc-stage3-{debug,release}-{build,tests}.log`,
`build/rpc-stage3-python-windows-retry-build.log`, `build/rpc-stage3-python-windows-tests.log`.
WSL logs: `/home/vinc-test/python-module-deploy-20261105/rpc-stage3-{configure,build,tests}.log`.
The Windows Python fixture was test-only: BUILD_TESTING/root variables were supplied by
its external parent; PATH selected its existing CPython 3.13 rather than MinGW Python.
A transient MSVC C1090 PDB-service failure was followed by a successful two-job rebuild.
No production toolchain/runtime protection was relaxed.

This stage-3 record is focused source acceptance, **not** full-project or clean installed-SDK
acceptance. Subsequent installed-consumer coverage is recorded in [stage 4](#integration-acceptance).
Native macOS remains in root TODO. Historical stage-1/2 records below retain their original scope.

第三階段紀錄僅涵蓋來源版重點回歸；後續乾淨 SDK 與搬移部署結果見第四階段，
不回溯擴大較早紀錄的驗證範圍。原生 macOS 仍待驗。

<a id="synchronous-calls"></a>

## Synchronous calls (stage 2) / 同步呼叫（第二階段）

```cpp
auto messages = vinc::make_rpc_messages<GetCountRequest, GetCountResponse>();
auto result = context.call_rpc("receiver.get_count", messages);
if (!result) {
    // Framework failure: the previous response is unchanged.
    report(result.error());
    return;
}

// Nonzero application status still has a valid typed response.
use_response(*result, messages.response());
```

- One deadline covers snapshot/admission and both transport phases; default five seconds.
  This is not a hard scheduling/allocation bound. No nested pump, extra control service
  or extra worker thread is introduced. The target still pumps its registered handler.
- Host routes Grant/Reply by request ID and wakes the specific caller. That caller copies
  shared memory and validates schema/layout; watchdog never moves large payloads.
- `BlockingCallInPump` (18) rejects synchronous calls on the same Context's pump thread;
  `SynchronousSelfCall` (19) rejects its own Module. Checks precede snapshots, request IDs
  and leases. Async calls from callbacks remain legal; another thread may synchronously
  call, without promising freedom from application deadlocks.
- Pump ownership lives in Context, not DLL-local TLS. Plugin entry ABI is now **16**:
  rebuild Host, Plugins and bindings. RPC wire v1 and bootstrap v16 are unchanged.
- Sync and async share configured pending/request-byte budgets. A timeout retains a bounded
  Host record until late Grant release or submitted target completion. Host progress cleans
  up without caller pump; backpressure keeps the record for retry. No request retry or
  early reuse of a live target's slot occurs.
- Stop wakes waiters with `Closed`. Stopped progress does not send cleanup as a new fault;
  central recovery waits for actual process exit. Runtime destroys Host records before
  unloading the Plugin, including retained budget control blocks. Module-owned threads
  must still finish their Context calls before Module destruction; no detached-call lifetime
  extension is promised.
- Only a validated typed response replaces the previous response. Framework failures preserve
  it; nonzero RpcStatus is an application outcome with a valid response. Do not concurrently
  mutate/use the same pair. Admitted synchronous outcomes count once in rpc_stats completions,
  but never count as callbacks in a pump batch. Late replies do not count again.

同一 Context 的 pump 執行緒內同步呼叫、自我同步呼叫都在送出前拒絕；另一條執行緒可呼叫。
逾時不代表遠端沒執行，不自動重試；晚到事件由既有監督流程回收，不要求補 pump。
停止後交由中央確認程序退出才回收，不能搶回活槽位。Plugin entry 16 必須整套重建。

### Stage 2 validation / 第二階段驗證

`rpc.synchronous` uses a real Plugin: no-pump success, reuse, large payload, application
failure, old-response preservation, unavailable/mismatched services, self-call rejection,
service/completion/Host-dispatched Shell hook guards, restored scope, concurrent sync calls
and async completion dispatch. `rpc.synchronous_target_exit` checks actual target death.
`rpc.synchronous_transport` uses real control sessions/shared memory with controlled delivery
for late Grant/Reply, duplicate Grant, timeout before/after Submit, stop wakeup, malformed
response and shared/bounded admission budgets.

真實 Plugin 測試驗證不 pump 同步完成與跨 DLL 防護；受控測試刻意延後 Grant／Reply，驗證
逾時保留占用、晚到可回收、停止喚醒、共用預算及錯誤回覆不破壞舊資料。初次測試發現正常
返回後 deferred Release 被誤報 Closed，已修正停止後 progress；中央退出回收契約維持。

2026-11-06 final validation (incremental source builds):

| Environment | Result |
| --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Debug | All nine `rpc.*` plus `module.context`, `module.plugin`, `module.manager_faults`, `module.runtime_faults`: **13/13** (51.59 s); `sdk_header_smoke` built. |
| Same Windows toolchain, Release | Same **13/13** (46.55 s); `sdk_header_smoke` built. |
| WSL2 Ubuntu 24.04 x64, GCC/libstdc++ 13.3.0, CMake 3.28.3, Release | Same **13/13** (16.76 s), native-filesystem source copy; `sdk_header_smoke` built. |

Build targets: `rpc_process_tests`, `rpc_synchronous_tests`, `rpc_contract_tests`,
`rpc_recovery_tests`, `rpc_client_foundation_tests`, `module_context_tests`,
`module_plugin_tests`, `module_manager_fault_tests`, `module_runtime_fault_tests`,
`sdk_header_smoke`. Run:

```sh
ctest --test-dir <build> -C <configuration> -R '^(rpc[.]|module[.](context|plugin|manager_faults|runtime_faults)$)' --output-on-failure
```

Windows logs: `build/rpc-stage2-{debug,release}-{build,tests}.log` (header builds also
ran earlier in this stage). WSL logs:
`/home/vinc-test/python-module-deploy-20261105/rpc-stage2-{build,tests}.log`.
These were stage-2 focused regressions, not a full-project CTest run. At that stage, native macOS, Python adapters
and clean installed SDK remain outstanding / 非全專案測試；原生 macOS、Python 與乾淨
installed SDK 留待後續驗收，Mac 清單已記入根目錄 TODO。

<a id="client-foundations"></a>

## Client foundations (stage 1) / Client 基礎（第一階段）

- `<vinc/rpc/messages.hpp>` supplies `vinc::rpc::RpcMessages<Request, Response>` and
  `vinc::make_rpc_messages<Request, Response>()`. Generated messages own independent
  process-local storage. Copy construction deep-copies; move construction follows
  Message Storage; whole-pair assignment is disabled, while individual message `assign()`
  remains available. This helper alone performs no RPC.
- `<vinc/rpc/rpc_client.hpp>` supplies the movable/noncopyable typed handle, local Request
  creation, single-pending Busy checks, default/per-call options, snapshot submission,
  typed response validation and close/move callback lifetime rules.
- `detail/address.hpp` parses exactly `module.service`, retaining the existing 128-byte
  limit for each segment. Extra dots/slashes, empty segments, invalid ASCII/NUL and
  oversized names are rejected before allocation of the owned segments.
- `detail/rpc_client_access.hpp` is the designated Context factory boundary.
  `detail/CallEndpoint` is a submission adapter, not another transport or service.
  Context issuance is connected in stage 3; tests may supply a controlled endpoint.
  Applications must not implement this detail endpoint to bypass Context identity.
- The endpoint owner revokes admission and protects entered submissions at teardown. The handle retains only a weak endpoint reference. Closing
  suppresses dispatch not yet begun, releases application captures outside its mutex,
  and leaves transport cleanup to the endpoint. Old completion tokens cannot clear a
  newer call's Busy state. A typed handle's local state/token allocations do not promise
  allocation-free operation; the existing STL/allocator exception policy is unchanged.
- `Busy = 17` is appended to the RPC error catalog; previous numbers and wire formats
  are unchanged. The existing raw reply definition is shared with the new foundation;
  existing asynchronous wire behavior remains unchanged.

第一階段僅完成本機型別、名稱解析與 handle 基礎，不提供假的同步成功或替代 Host。
`RpcClient` 可作為空的 Module 成員，空／已關閉 handle 的 call 回 Closed；
Context factory 與 Python 現已於第三階段接上；同步傳輸與 pump 防護見上方第二階段。
下列第一階段測試紀錄保留當時範圍，不代表後續階段未完成。

### Validation / 驗證

`rpc.client_foundations` uses a controlled submission endpoint to verify malformed and
boundary addresses, local-only creation, generated types, independent message copies,
Request snapshots, default/override options, Busy, repaired admission/exception retry,
typed application failures, malformed/schema-mismatched replies, preserved error chains,
duplicate results, callback resubmission, move/close/destruction and expired endpoints.
It does not exercise production Host/Context progress or prove cross-process recovery.

2026-11-06 validation (incremental source trees, no installed-SDK acceptance):

| Environment | Build / tests |
| --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Debug | `rpc_client_foundation_tests`, `rpc_contract_tests`, `sdk_header_smoke` built; two CTests **2/2** passed. |
| Same Windows toolchain, Release | Same three targets built; two CTests **2/2** passed. Release tests were explicitly enabled locally; the project default remains unchanged. |
| WSL2 Ubuntu 24.04 x64, GCC/libstdc++ 13.3.0, CMake 3.28.3, Release | Same three targets built from the updated native-filesystem test copy; two CTests **2/2** passed. |

Commands: build those three targets, then run
`ctest --test-dir <build> -C <configuration> -R '^rpc[.](client_foundations|contracts)$' --output-on-failure`.
Windows logs: `build/rpc-stage1-{debug,release}-{build,tests}.log`.
WSL logs: `/home/vinc-test/python-module-deploy-20261105/rpc-stage1-{build,tests}.log`.

Initial Debug compilation caught an incorrect test use of `Error::is` for an operation;
it was corrected to `is_operation` before these passing runs. A sandboxed Windows
`rpc.contracts` invocation was denied native shared-memory access; both native runs above
used the normal OS environment. The WSL mounted-source shell command was replaced with
a fixed script after argument expansion failed before copying/building.

Windows Debug／Release、WSL Release 各 **2/2** 通過，SDK 公開標頭獨立編譯亦通過。
僅驗證上述本機 foundations 與既有契約；未重跑全專案、完整跨程序 RPC、Python、
最低 MSVC 19.44、乾淨 installed SDK 或原生 macOS。本次結果不能當成同步傳輸已完成。
後續接線與 Mac 待驗項目已列入根目錄 TODO；完整
當時尚未完成的[驗收清單](CLIENT_CONTRACT.md#acceptance-checklist--驗收條件)，以第四階段紀錄更新目前狀態。

RPC is synchronous/asynchronous typed Module-to-Module request/response through inherited Context. The native official UI Module can use it; a browser-plugin RPC bridge remains future work.

RPC 提供 Module 間同步／非同步、具型別的 request / response，使用繼承自 BaseModule 的 Context。
官方 UI Module 可使用同一入口；瀏覽器 UI plugin RPC bridge 留到後續接線。

<a id="公開-api"></a>

## Public API / 公開 API

```cpp
context.register_rpc<Request, Response>(service, handler, options);
auto messages = vinc::make_rpc_messages<Request, Response>();
auto status = context.call_rpc("receiver.get_count", messages, call_options);
auto created = context.rpc_client<Request, Response>("receiver.get_count", completion, call_options);
// Check created before taking the handle.
auto client = std::move(*created);
auto request = client.make_request();
auto accepted = client.call(*request);
context.rpc_stats();
```

- Request/Response must be generated messages. Registration fixes both schema hashes;
  central checks them on every call. Addresses have exactly one dot; each segment uses
  ASCII letters/digits/_/-, up to 128 bytes. Registration uses only the local service name.
- `register_rpc(service, handler, options={})` returns Result<void>, waiting for central
  registration. Services live until Module stop. A concurrent pump may wait for local
  handler installation; the Host receiver remains independent.
- Handler: `RpcStatus(const Request&, Response&)`, with call-scoped references. Framework
  validates/copies the response after return. Nonzero application status may carry a valid
  Response; framework Result errors are distinct.
- `rpc_client(address, completion, options={})` returns Result<RpcClient>. Creation binds
  types/address/callback/default timeout locally, without requiring the target to exist.
  Store the movable, noncopyable handle as a Module member or other suitable owner.
- `client.call(request[, options])` returns Result<void> for local admission. Immediate
  rejection has no callback; admitted remote failures arrive on pump. Request is copied
  before returning. `make_request()` creates ordinary independent local message storage.
- Each client permits one outstanding call through the beginning of callback dispatch;
  a second returns Busy. Busy clears before callback entry, allowing resubmission there.
  Different clients share Context capacity limits. A timeout may free the handle while
  old transport cleanup still consumes capacity. No automatic retry occurs.
- Completion: `void(Result<RpcReply<Response>>)`. Each Response unique_ptr owns independent
  local storage and may be retained. `close()`/destruction suppress future dispatch and
  release captures; they neither cancel remote work nor join an already-running callback.
  Synchronize handle mutation/move/destruction and captured object lifetimes yourself.
- The handle owns only a weak endpoint. Context teardown revokes admission before its
  internal RPC Client disappears, including admissions that already retained the endpoint.
  This does not authorize detached threads to access a destroyed Context object.
- Synchronous `call_rpc(address, messages, options={})` returns Result<RpcStatus> and
  follows the no-caller-pump/deadline/guard contract above. Options are optional and trailing.

註冊只寫本地服務名；呼叫地址為 `module.service`。RpcClient 建立時不探測遠端；每次送出
才解析實際服務與 MID。請保存 handle，否則解構會抑制尚未開始的 callback。
每個 client 只容許一筆未派送結果，第二筆回 Busy；進入 callback 前解除，故可在 callback
再次呼叫。Request 返回前已複製，Response 交付獨立所有權。close 不等於取消遠端或立即
重用共享槽位；逾時後仍可能等待安全回收。同步 call_rpc 不經 caller pump，失敗保留舊 Response。

See the [complete example](../../../examples/rpc/README.md).

完整範例見 [examples/rpc](../../../examples/rpc/README.md)。

<a id="pump佇列與失敗"></a>

## pump, queues, and failures / pump、佇列與失敗

- Handlers/completions run only on the pump caller. A pass consumes the Host RPC control events ready at its start; events arriving during callbacks wait for another pass, not every occupied slot.
- Each service dispatches in central Submit-receive order, not global call-start FIFO. Handlers follow received Invoke order; no global inter-service chronology.
- QueueFull means not accepted and no handler. Shared-slot/local-pending/dispatch limits may cause it. Retry later explicitly; no automatic retry.
- Synchronous calls follow the stage-2 contract above and are rejected on the pump thread. Handlers may issue async calls. Slow callbacks delay the pass, without preemption/hard real-time guarantees.
- For asynchronous calls, the deadline starts at call and is checked by pump; no pump means no timeout callback. Even an arrived-but-unconsumed reply may time out with unknown outcome. Notify once.
- Unsubmitted grants may release; submitted handlers may continue. No public cancel. Timed-out bookkeeping/slots remain until safe completion/confirmed exit; late replies only reclaim. Timeout does not prove nonexecution or exactly-once through crashes.
- Confirmed target exit with unprocessed completion gives TargetExited; exit/completion races need not preserve the response. Heartbeat timeout cannot reclaim live leases. Stop closes admission and can fail pending calls.
- Escaping user exceptions follow Module fault policy; central/peers continue. Bad protocol closes the offending Session; existing supervision stops it.

- Handler／completion 只在 pump 呼叫執行緒執行。一輪先取出當時已到達 Host 的 RPC 控制事件；
  callback 期間新增的事件留到下一輪。處理的是本輪開始時已就緒的工作，不等待每個已占用槽位。
- 每服務依中央接收 Submit 次序排送；不同 caller 的本地 call 開始時間不構成全域 FIFO。
  Handler 依 Invoke 收到順序執行，不承諾所有服務間的時間排序。
- QueueFull 表示未被接受，不執行 handler。共享槽位、本地 pending 或排送預算不足均可能回報。
  使用者可稍後重試，框架不自動重試。
- 同步 API 遵守上述第二階段契約，在 pump 執行緒內拒絕；handler 仍可發 async RPC。
  慢 callback 延後整輪，沒有搶占或硬即時保證。
- 非同步 Deadline 從 call 開始，在 pump 檢查。未 pump 不會自行執行逾時 callback；即使回覆已到達但
  尚未被 pump 消費，也可能回報 TimeoutOutcomeUnknown。到期只通知一次。
- 尚未提交的 grant 可釋放；已提交的 handler 可能繼續。第一版沒有公開取消 API。
  timeout 保留簿記／槽位到安全完成或確認程序退出；晚到結果只回收，不再次通知。
  逾時不等於未執行，不承諾跨程序崩潰的 exactly-once。
- Target 確認退出且完成尚未處理時回報 TargetExited；退出與完成競爭不保證取得結果。
  heartbeat timeout 不足以回收存活對端的 lease。正常停止封住新工作，未完成呼叫也可能失敗。
- 逃出 handler／completion 的例外依 Module 故障政策處理，中央與其他 Module 繼續。
  非法協定回報 Protocol／關閉違規 Session，由既有監督負責停止與退出。

<a id="共享槽位與所有權"></a>

## Shared slots and ownership / 共享槽位與所有權

Central creates a fixed pool at registration: each slot has request/response regions plus two SharedWords, total (align8(request_bytes)+align8(response_bytes)+16)*slots. Capacity includes complete images/metadata, not only arrays.

中央在服務登記時配置固定池，每格包含 request 區、response 區和兩個 OSAL SharedWord。
大小為 (align8(request_bytes) + align8(response_bytes) + 16) * slots。
容量包含完整訊息映像與 metadata，不只是陣列內容。

```text
Free → Granted (caller writes request)
     → Queued  (callee reads request and writes response)
     → Reply   (caller copies response)
     → Free    (caller sends Release)
```

Socket carries schema, MID-bound identity, slot descriptors, status, and notifications; all message payload uses shared memory. Mapping checks bounds/generation; SharedWord publication precedes Storage layout validation. This RPC copies data, not a user-editable shared loan.

Socket 只傳型別、經 MID 認證的身分、槽位描述、狀態及通知；所有 message payload 都走共享記憶體。
每次映射檢查邊界／世代，以 OSAL SharedWord 確認映像發布後再由 Message Storage 驗證布局。
這是有複製的 RPC，不是使用者直接編輯共享區的 loan API。

Scoped request/service IDs and slot tokens increase without wrap; restarted Modules receive new MIDs. Slots are never overwritten and duplicate notifications do not rerun handlers. Central names/resource records govern cleanup. A live peer lease prevents early reuse even if the other peer dies; reclaim dead ownership only after OS exit. Central death follows existing child-liveness policy, not a sandbox against malicious shared-memory writes.

各範圍 request ID、service ID、slot token 單調增加且不回繞；新 Module 使用新 MID。
槽位不覆寫，重複通知不重跑 handler。共享名稱沿用中央資源紀錄產生及回收。
存活對端持有讀寫 lease 時，即使另一端已死也不提早重用；中央確認 OS 退出才回收死亡方。
中央死亡依既有生存契約結束子程序，不承諾對惡意改寫共享記憶體的 Module 提供沙箱。

<a id="資源與執行緒"></a>

## Resources and threads / 資源與執行緒

Settings stay in vinc/config.hpp, without Param migration:

設定集中於 vinc/config.hpp，本輪未遷入 Param：

| Setting | Default/use |
| --- | --- |
| rpc_request_bytes / rpc_response_bytes | 64 KiB each, ServiceOptions overrides |
| rpc_slots / rpc_slot_limit | 8 per service, range 1–256 |
| rpc_shared_bytes | 256 MiB centrally across pools |
| rpc_local_bytes | 64 MiB outbound snapshots per Context |
| rpc_pending_limit | 64 unreclaimed calls including timeout tombstones |
| rpc_service_limit | 256 services including lease-draining services |
| rpc_active_limit | 1024 granted slots centrally |
| rpc_control_limit | 4096 per central/Host/peer queue |
| rpc_name_bytes / rpc_reason_bytes | 128 / 256 bytes |
| rpc_call_timeout / rpc_max_call_timeout | 5 s default / 24h max; default for registration |
| rpc_control_retry | 5 ms dispatch/recovery checks |

| 設定 | 預設與用途 |
| --- | --- |
| rpc_request_bytes / rpc_response_bytes | 各 64 KiB，ServiceOptions 可覆寫 |
| rpc_slots / rpc_slot_limit | 每服務預設 8 格，範圍 1～256 |
| rpc_shared_bytes | 每中央 256 MiB，全部服務配置總和 |
| rpc_local_bytes | 每 Context 64 MiB，保留 outbound request 快照總額 |
| rpc_pending_limit | 每 Context 64 個未回收 call，含逾時 tombstone |
| rpc_service_limit | 每中央 256 個服務，含等待 lease 回收的服務 |
| rpc_active_limit | 每中央 1024 個已借出槽位 |
| rpc_control_limit | 中央 inbox、各 Host／peer 控制佇列各 4096 筆 |
| rpc_name_bytes / rpc_reason_bytes | 128 / 256 bytes |
| rpc_call_timeout / rpc_max_call_timeout | 預設 5 秒，最大 24 小時；登記等待採預設值 |
| rpc_control_retry | 中央 worker 排送重試／退出補查間隔 5 ms |

These are not RSS limits: handlers, serialized temporaries, Python copies, and retained responses add memory. Pools are fixed; each transfer opens/maps them without a mapping cache.

這些不是程序整體記憶體上限：本地 handler 訊息、序列化暫存、Python copies 及使用者保留的
Response 額外占空間。共享池固定，每個 transfer 開啟／映射該池，尚無 mapping cache。

One central RpcManager worker handles allocation/dispatch/recovery outside lifecycle supervision. Host retains its sole watchdog/control receiver; Plugins own no second service or RPC-callback thread. Idle wakeups and runtime scheduling exist; no fixed latency or guaranteed advantage over Param is claimed.

中央增加一個 RpcManager worker 處理配置、排送、回收，避免共享區配置卡住生命週期監督。
ModuleHost 沿用唯一 watchdog／控制 receiver；Plugin 沒有第二套控制服務，也沒有 RPC callback thread。
worker 有少量閒置喚醒，傳送受現有 runtime 排程影響，不宣稱固定延遲或必然比 Param 快。

rpc_stats reports local pending/request_bytes/queue_full/timeouts/requests/completions. Requests counts decoded/handler-stage work including layout/capacity failures, not application success. Completions counts asynchronous notifications and admitted synchronous outcomes once, including timeout but not late replies. These are not central-wide traffic totals.

rpc_stats 返回本地 pending、request_bytes、queue_full、timeouts、requests、completions。
requests 計入服務解碼／handler 階段已處理的請求（含布局／容量失敗），不代表應用成功次數；
completions 是非同步通知及已接收同步呼叫的返回次數，timeout 計一次、晚到回覆不重算。不是全中央流量統計。

<a id="python"></a>

## Python / Python 介面

```python
context.register_rpc(Request, Response, "service", handler)  # optional trailing ServiceOptions
options = vinc.rpc.CallOptions()
options.timeout_ms = 5000
client = context.rpc_client(Request, Response, "receiver.service", completed, options)
request = client.make_request()
client.call(request)                    # optional per-call CallOptions override
messages = vinc.make_rpc_messages(Request, Response)
status = context.call_rpc("receiver.service", messages, options)
response = messages.response()
```

Python mirrors C++ address, Busy, close, snapshot, timeout and pump-guard semantics.
Framework failures raise VincError for synchronous calls/local admission. Async completion
`completed(reply, error)` receives `(RpcStatus, Response), None` or `None, VincError`.
Handlers return RpcStatus; completions return None. Unhandled exceptions preserve Python
diagnostics and fault that Module. Types must be compiled-in; there is no schema interpreter.

Synchronous native waits release the GIL, never run nested callbacks, and retain Context
admission until return. Python serialization/deserialization and callback invocation hold
the GIL. Only successful typed decoding replaces `messages.response()` with a new Python
object; old Python references keep the old response, and framework failure preserves it.
Do not concurrently mutate/use one pair. Retained client wrappers cannot keep native Context
alive; teardown clears tracked callback captures, including bound-method cycles.

Python 與 C++ 共用呼叫狀態機及 Host，不增加控制服務或背景 callback 執行緒。
同步等待釋放 GIL；序列化、解碼、callback 才持有 GIL。同步成功後 response() 指向新的
獨立物件，先前保存的 Python 引用仍指向舊物件；框架失敗不替換 Response。
callback 內同步呼叫在送出前拒絕，非同步重送允許。未處理例外只讓該 Module 失敗。

<a id="相依協定與-abi"></a>

## Dependencies, protocol, and ABI / 相依、協定與 ABI

- vinc::rpc supplies Context facade/image validation/read/write using Storage, Common, OSAL, control codecs.
- vinc::rpc_host owns Host Session integration and is not linked into Plugins as a control service.
- vinc::rpc_manager owns central worker/services/pools.
- ErrorDomain::Rpc=14 has a build-composed static catalog; numeric errors/operations/causes are encoded, never pointers/layouts. Preserve unknown codes; reject malformed/incompatible formats.
- Application domain 6, transfer/admission v1; error envelope versions are separate.
- Plugin entry 16/bootstrap v16 require matched Host/Plugin/SDK rebuilds and toolchain/stdlib/config/layout/CRT.
- RPC does not publish topics or fill Bus publisher/count/timestamps. Applications needing those concepts define payload fields instead of borrowing read-only Bus metadata.

- vinc::rpc：Context facade、布局驗證／映像讀寫，依賴 Storage、Common、OSAL、控制 codec。
- vinc::rpc_host：Host Session 接線；Plugin 不連入此控制服務實作。
- vinc::rpc_manager：中央 worker、服務與池所有權。
- ErrorDomain::Rpc = 14，靜態 catalog 經 CMake 整合。錯誤／操作及原因鏈明確編碼，不傳送
  C++ pointer 或物件布局。未知錯誤代碼保留數值，損壞／不相容格式拒絕。
- RPC domain = 6，transfer／admission 版本 = 1，錯誤 envelope 另有版本。
- Context 布局已改：Plugin entry = 16、bootstrap = v16，Host、Plugin、SDK 須全部重新建置。
  保留相同工具鏈、標準庫、Debug/Release、布局與 CRT 限制。
- RPC 不作為 topic 發布，不替訊息填 Bus publisher、topic 序號或發布時間；需要時應用須自行
  在 message 資料欄位定義，不借用唯讀 Bus metadata。

<a id="驗證"></a>

## Recorded validation / 驗證

Tests cover real large C++ cross-process payloads, Python↔C++, schemas/application failures/QueueFull/timeouts/late replies/target crash/fixed pump batches/duplicates/capacity/errors/recovery. Submit/Complete allocation faults verify leases do not transfer before control queue acceptance. Recorded 2026-10-14 on Windows x64/MSVC 19.51/CPython 3.13:

tests 包含真實 C++ 跨程序大訊息、Python↔C++、schema mismatch、應用失敗回覆、QueueFull、
timeout／late reply、目標 crash、固定 pump batch、重複通知、容量／錯誤協定與故障回收。
recovery 亦注入控制 payload 配置失敗，確認 Submit／Complete 未成功入列時不提早轉移 lease。
2026-10-14，Windows x64／MSVC 19.51／CPython 3.13：

- Full Debug/Release builds, SDK single-header and C++ examples passed.
- Release full **85/85**; Debug initially **84/85** because concurrent tests launched the same Vinc name and hit exclusivity. Isolated examples.user_module passed; exclusivity was unchanged.
- After final allocation fix, six RPC suites plus Manager faults/example passed **8/8** in both.
- Recovery injection checked reclaiming untransferred Submit/Complete leases.
- Real Shell examples produced expected C++→Python, Python→C++, C++ self-call replies.
- Logs: build/rpc-{debug, release}-tests.log and build/rpc-{debug, release}-final-tests.log.

- Debug／Release 完整建置通過，包含 SDK 單標頭與 C++ 範例。
- 全專案 Release 85/85；Debug 初輪 84/85，唯一失敗為兩組並行測試啟動同名 Vinc，
  觸發既有排他鎖。單獨重跑 examples.user_module 通過，未修改排他規則。
- 最後的配置失敗修正後，RPC 六套＋ModuleManager 故障＋使用範例，兩組態各 8/8 通過。
- RPC recovery 注入 Submit／Complete payload allocation failure，檢查未交接的 lease 可回收。
- 實際從 Shell 啟動 examples/rpc，C++→Python、Python→C++、C++ self-call 均收到預期回覆。
- 驗證紀錄：build/rpc-debug-tests.log、build/rpc-release-tests.log、
  build/rpc-debug-final-tests.log、build/rpc-release-final-tests.log。

## macOS 來源補驗（2026-11-11）

目前 entry-19 的 Debug／Release 全部十項 `rpc.*`（含 Python）、Context／Plugin／故障
與來源 Python 回歸已完成，見[第二階段主報告](../module_system/MACOS_SOURCE_FUNCTIONAL_VALIDATION.md)。
這項後續結果取代上方歷史紀錄的來源待驗狀態；後續[第三階段](../sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md)
已補 Debug／Release 乾淨 SDK-only consumer／application／python 與搬移公開標頭 smoke。

<a id="待處理事項"></a>

## Outstanding issues / 待處理事項

- Browser bridge/authorization/typed UI APIs remain unwired.
- No latency/throughput measurement; mapping-cache value needs evidence.
- Registration timeout can leave a central service without a locally obtained handler identity. No automatic retry/rollback; treat it as initialization failure and recover through Module exit, not assume same-name retries succeed.

- 瀏覽器 UI bridge、權限與 typed UI API 尚未接線。
- 未量測 RPC 延遲／吞吐量；mapping cache 是否值得增加，待量測。
- 登記逾時可能已在中央建立服務，本地未取得 handler 身分；不自動重試／回滾。
  應視登記失敗為初始化失敗，交由 Module 停止／退出回收，不保證同名重試成功。

Allocation injection covers payload allocations that can throw to callers. MSVC Debug vector default construction is noexcept yet allocates iterator metadata; failure terminates beyond outer catch. Existing STL/allocator policy remains, without a universal OOM-survival promise for central.

配置失敗測試只涵蓋可拋回呼叫端的 payload allocation。MSVC Debug 的 vector 預設建構子
宣告 noexcept，仍會為 iterator proxy 配置記憶體；該處配置失敗會終止程序，無法由外層 catch
復原。本輪維持既有 STL／allocator 例外政策，沒有承諾全域 OOM 仍可保住中央。

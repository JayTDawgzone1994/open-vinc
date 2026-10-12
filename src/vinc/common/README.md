# Common framework interfaces

正式入口為 `error.hpp`／`result.hpp`。ModuleSystem 遷移後已移除無呼叫端的舊整數錯誤表。

## Shared owner identities

`<vinc/owner_id.hpp>` defines the standard-library-only numeric boundary shared by
ModuleSystem and Concurrency: zero is unowned, 1 through 2^63−2 are Module owners,
and 2^63−1 is the central owner. ModuleSystem exposes its MID range in `mid.hpp`
and checks the allocator there; Concurrency validates without truncating values.
This is a fixed protocol boundary, not a runtime Param setting. One bit of the
64-bit shared control word remains available for atomic publication and release.
No registry or dependency on ModuleSystem is introduced in common.

`config::concurrency_retry_interval` defaults to 5ms. It controls cancellable
shared-mutex retry sleeps, not a maximum holding time; see the
[Concurrency contract](../components/concurrency/README.md).


## Error interface

`vinc::Result<T>` is `std::expected<T, vinc::Error>`. Success is its value state;
`Error{}` is an unknown failure, never success. Common owns `ErrorDomain`, the
value-only payload, typed construction/matching, catalog contracts and formatting
helpers. Each component owns its own `int32_t` error and operation enums.

```cpp
auto result = vinc::osal::IPv4Address::parse(text);
if (!result) {
    const auto& error = result.error();
    if (error.is(vinc::osal::ErrorCode::InvalidArgument)) {
        // Handle an invalid address according to the caller's policy.
    }

    report(vinc::error_to_string(error));
}
```

`Error` contains current-layer `domain`, signed `code`, signed `operation`,
`NativeError`, and a bounded array of numeric cause records.
Use component enums with `vinc::make_error()` or the typed Error constructor;
mixing a code with another domain's operation is rejected at compile time.
`is()` and `is_operation()` include the domain in the comparison. Raw fields and
`Error::from_raw()` are intended for validated transport adapters, including
unknown values. Copying the payload needs no allocation or category-object lifetime.

The sole text entry point is `std::string vinc::error_to_string(const Error&)`.
It may allocate/throw; do not parse its human-readable text as a protocol. Unknown
domains, codes, operations and native sources remain numeric. Domain zero is
unknown; operation zero omits operation context. Registered error codes are nonnegative;
registered operations are positive. Both use explicit stable assignments. Code zero
may identify a legacy failure and does not mean success. Never renumber/reuse published identifiers.

### Bounded cause chains

`config::error_chain_max_depth` in `<vinc/config.hpp>` defaults to **3**, counting
the current error plus two causes. Its minimum is **2**. It is a compile-time
layout setting, not a runtime Param: rebuild every linked C++ consumer with the
same value after changing it. The Error layout remains standard-layout and
trivially copyable, including for existing Windows native I/O containers; neither
property makes it a wire format or a stable binary ABI.

At an unrecoverable component boundary, wrap the lower error with the current
component's code and operation:

```cpp
if (!child) {
    return std::unexpected(vinc::wrap_error(
        vinc::output_hub::ErrorCode::SpawnFailed,
        vinc::output_hub::ErrorOperation::Spawn,
        child.error()));
}
```

- `wrap_error()` adds one current layer and inherits the lower error's native
  diagnostic. Construction, copying and wrapping allocate no memory and own no
  pointer. `std::unexpected(existing_error)` forwards the chain unchanged.
- `record()` returns a copy of the current numeric record; `causes()` returns a
  read-only span from nearest to deepest cause. The span borrows the owning Error
  and expires when it is destroyed or changed; temporary Error access is rejected.
  `cause_storage` is visible only to preserve standard layout; consumers use the
  accessors, not that implementation member.
- `is()` and `is_operation()` inspect only the current layer. Equality compares
  the complete stored chain and truncation state.
- When full, retain the new current layer, the nearest causes that fit, and the
  deepest cause. With capacity 3, wrapping `C <- B <- A` into D yields
  `D <- C <- [intermediate causes omitted] <- A`. With capacity 2, only the
  current and deepest records fit. `truncated()` remains true after further wraps.
- `error_to_string()` formats each retained record through the static catalog,
  placing the omission marker immediately before the deepest cause. It may
  allocate/throw; a single-record error keeps the previous text format.
- Validated transport adapters may call `Error::from_raw()` with numeric current
  fields, a nearest-first cause span and a truncation flag. A truncated input must
  include at least its deepest cause; any existing omitted section must be before
  that final cause. Larger incoming arrays are bounded using the same retention
  policy. This factory is not a protocol validator.

The chain describes one failure's propagation. Independent cleanup failures are
not automatically causes of the original failure; their reporting policy requires
a separate component decision.

Windows x64 MSVC measurements (bytes; Debug and Release agree at default capacity):

| Configuration | `Error` | `Result<void>` | `Result<int>` |
| --- | ---: | ---: | ---: |
| Before cause chains | 32 | 40 | 40 |
| Capacity 2 | 80 | 88 | 88 |
| Default capacity 3 | 112 | 120 | 120 |
| Capacity 5 | 176 | 184 | 184 |

The fixed diagnostic storage increases Result size even on the success path.
These are measured object sizes, not throughput measurements or cross-platform
size guarantees. Capacity 2/5 tests rebuild isolated copies of common; capacity 1
must fail the minimum-capacity assertion.

### Component catalogs and CMake composition

`cmake/VincMsvcDependencies.cmake` is a build-only helper included by Common.
For Windows MSVC/Ninja it probes the localized `/showIncludes` output explicitly
as UTF-8 and verifies a known Unicode header name before accepting the prefix.
The deferred update runs in the top-level CMake directory, after compiler-cache
reloads, so external `add_subdirectory` consumers receive the same correction.
An unverified encoding fails configuration with a probe log; it is not silently
treated as a usable dependency prefix. Use a UTF-8 developer console/toolchain.
Other compilers and generators are unchanged. This adds no runtime dependency.
Existing objects with missing header dependencies require one clean rebuild.
Reproduction, regression tests and platform limits are maintained in the
[SDK build notes](../sdk/README.md#msvcninja-增量建置修復2026-10-06).

- `include/vinc/error_catalog.hpp` defines immutable C++ catalog contracts and
  constexpr validation. Component `error.hpp` headers own enums/traits; their
  `detail/error_catalog.hpp` headers own descriptions and local table objects.
- `cmake/VincErrors.cmake` registers a provider target, literal include path and
  qualified C++ catalog symbol once per component. CMake never parses enum values
  or maintains individual error descriptions.
- Provider targets expose only error metadata and optional native formatting
  code. They depend on common, never on their runtime component or `vinc::errors`.
- `vinc_finalize_error_catalog()` freezes registrations and writes a configure-time
  input under the build directory. A Common-owned build rule creates the C++
  translation unit from it, including after clean or deletion of that source.
  Finalization must run exactly once, after all providers. The generated static
  array is checked by `static_assert`.
- `vinc::common` has no dependency on a component or on `vinc::errors`.
  `vinc::errors` provides the public formatter definition and depends on common
  and metadata providers. There is one selected catalog per build, with one
  formatter definition per final program. No runtime registration, mutable map,
  or static registration constructor is used.

Example provider registration (declarations/descriptions stay in C++):

```cmake
vinc_register_error_catalog(TARGET vinc_osal_errors
    HEADER vinc/osal/detail/error_catalog.hpp
    SYMBOL vinc::osal::detail::error_catalog)
```

Applications using only common must link `vinc::common` and `vinc::errors` for
text queries. `vinc::osal` propagates both, as does the SDK through its component
dependencies. A standalone OSAL build supplies its own minimal catalog and
needs only the sibling common directory; it does not pull in other runtimes.
Standalone common supports an empty catalog with numeric fallback. A custom
parent composing common/OSAL must call the finalizer after registering providers;
the full framework finalizes its built-in providers in `src/vinc/CMakeLists.txt`.
Late/duplicate registration and missing/repeated finalization are configuration errors.

C++ validation rejects duplicate domain/code/operation entries, mismatched entry
domains and empty descriptions. C++23 cannot enumerate arbitrary enum members:
coverage of enum members omitted from a table needs component tests. OSAL tests
cover its existing contiguous 23 code and 45 operation assignments. Future sparse
catalogs must provide their own appropriate coverage check.

### Native identity and process boundaries

`NativeError` stores a source namespace and an `int64_t` value, including negative
resolver codes and unsigned 32-bit Win32/NTSTATUS values. It contains no
`std::error_code` or process-local pointer. Native namespaces distinguish Win32,
Winsock, NTSTATUS, Linux/Darwin errno/resolver and platform generic-category values.
The current POSIX numeric namespaces target the project's x86-64 native ABI;
additional architectures require validation of the numeric namespace before reuse.

The OSAL metadata provider formats supported host-native sources. Foreign or
unsupported sources keep their number; NTSTATUS currently uses numeric fallback
and is never interpreted as a Win32 code. Standard thread-creation exceptions
are converted at the existing boundary. Unrecognized std::error_category values
have no portable category identity and retain only the number under unknown source
UINT32_MAX; arbitrary third-party categories need an explicit stable adapter.

Never memcpy Error or Result objects into an IPC frame. Each transport must specify
version, widths, byte order, envelope validation and unknown-value behavior.
`osal.error_process` demonstrates explicit field encoding in a bounded test-only
shared-memory protocol. Fixture version 2 includes record count and truncation
flags followed by numeric records; decoding validates version, flags, count and
length before reconstruction. The child exits before its parent reads and formats
errors, including unknown nested records and truncated chains. A synthetic larger
sender checks bounding on receipt. This is not a production codec.
This round does not change production Module/Message Bus wire formats or promise
a cross-version C++ ABI.

### Layer translation and migration status

Each layer first attempts its permitted recovery; an unrecoverable lower-layer
failure is translated into that layer's public code/operation. Callers need not
interpret lower-layer codes. Use `wrap_error()` to preserve bounded causes and native
provenance; do not silently turn all failures into one generic code.

- OSAL is fully on the common Result/Error contract and owns its 23/45 identifiers;
  caller-facing operations and internal causes follow the [OSAL contract](../osal/README.md#統一錯誤介面).
- OutputHub owns a small catalog and wraps OSAL capture/spawn failures with its own
  code/operation, retaining the OSAL cause for diagnostics.
- Control Channel and ModuleSystem use common Result/Error, component catalogs and
  bounded wrapping. Module v3 explicitly transports numeric error chains; hooks,
  manager creation, micro-programs and convenience adapters use the same contract.
- Message Bus uses common Result/Error throughout registration and publication,
  with a static catalog and bounded Storage/OSAL/Control Channel causes. Bus v2
  control responses transport numeric cause chains; the legacy payload/adapter is removed.
  See its README and DESIGN for version rejection and transport validation.
- Message Storage uses common Result/Error and a static catalog, including its original
  CapacityExceeded code zero. Catalog validation permits zero for error codes;
  operation zero remains unspecified. Success is represented only by Result's value state.
  Internal expected failures now propagate Result directly; allocation boundaries retain
  bad_alloc/length_error conversion. Public checked operations return Result; invalid unchecked
  access terminates through the common contract handler. AccessError has been removed.
  See the Message Storage README for scope and verification.
- Concurrency uses common Result/Error and a static metadata provider. SharedMemory
  failures are translated into Concurrency codes with bounded OSAL causes; local
  failures remain single-layer. See the Concurrency README for identifier migration.
- Shell uses common Result/Error for factories, parsing, routing, socket startup,
  terminal/service failures and script launch. ModuleSystem/OSAL/OutputHub failures
  retain bounded causes; script completion separates its Result from bounded Tcl
  diagnostic text. Its static catalog has no Tcl runtime dependency. See the Shell
  README for application return-code semantics and Tcl return-options handling.


### Error catalog migration verification

Verified on 2026-10-04 with Windows x64, MSVC 19.51.36257, Windows SDK
10.0.26100.0, CMake 4.3.2 and Ninja 1.13.2:

- Full Debug and Release builds, including public-header compilation: passed.
- Full CTest suites: **30/30** in each configuration.
- Clean standalone OSAL Release: build passed, **11/11** tests passed.
- Clean standalone common Release with no OSAL provider: build passed, **2/2** tests passed.
- `common.error_catalog` builds/runs a valid isolated consumer and checks seven
  deliberate failures: duplicate code/domain, mismatched domain, duplicate/late
  registration, missing/repeated finalization. Failures must have the expected diagnostic.
- `osal.error_process` passed with a real child process, explicit byte encoding,
  native and unknown values, post-exit local lookup, and truncated/version-invalid records.
- OutputHub integration verifies that failed spawn reports its own domain/code
  while retaining native provenance. Existing process, control and bus suites passed.
- NTSTATUS tests use synthetic failure values; no actual BCryptGenRandom failure
  was injected. Linux/macOS source was updated but not built or executed on those hosts.

Build-time catalog pointers and native formatter callbacks remain process-local.
The passing fixture does not establish a production cross-version error wire ABI.

### Cause-chain verification (2026-10-04)

Using the Windows x64 toolchain listed above:

- Full Debug and Release builds, including SDK public-header compilation: passed.
- Debug full CTest: **32/32** passed.
- Release full CTest initially passed **31/32**; the catalog consumer reused an
  object built against the previous Error layout. Its build log showed only the
  common formatter rebuilt while the consumer object retained its earlier
  timestamp. Isolated catalog/capacity fixtures now clean, configure and build in
  that order, so each run tests a consistent layout and recreates configure-time
  sources. The final Debug and Release common suites each passed **4/4**, including
  the previously failing catalog fixture. Together with the full runs, all 32 tests
  have passed in each configuration; the Release result includes this focused rerun.
- Clean standalone OSAL Release (including common): **13/13** passed.
- Cause tests cover typed wrapping, unchanged forwarding, bounded order, deepest
  cause retention, repeated truncation, owning copies, unknown values and text.
  Capacities 2 and 5 rebuild/run independently; capacity 1 must fail with its
  specific static assertion diagnostic.
- The real child-process fixture checks nested and truncated records after child
  exit. Malformed envelopes and a simulated larger-capacity sender are also checked.
- OutputHub failed-spawn integration resolves both the outer OutputHub catalog and
  the retained OSAL cause. Capture-read failure uses the same wrapper; this round
  does not inject a native capture-read failure.
- Linux/macOS were not built or run. Production IPC formats and ModuleManager's
  existing error contract are unchanged by this work.

## 待處理事項

### 2026-10-05 Common 重審修正

- **已修正：catalog 的 clean/build 重建缺口。**
  原先跨目錄加入 `file(GENERATE)` 來源，Ninja 只留下空的產生指令，clean 後不能恢復。
  現在 finalizer 以 `file(CONFIGURE)` 保存靜態 catalog 輸入，Common 自己的 CMake
  同時擁有 build-time 來源產生規則與 consuming target。無新增 runtime 相依或 registry。
  `common.error_catalog` 已加入 clean-first、單獨刪除生成來源後重建，以及切換 catalog
  symbol 後不 clean 的增量建置／查詢驗證；保留原有七種負向配置／編譯案例。
- **已修正：code／operation 表可混用。**
  原本的 `ErrorEntry` 改為 `ErrorCodeEntry` 與 `ErrorOperationEntry`，各自只接受
  對應的 enum trait；`ErrorCatalog` 的兩個 span 也有不同元素型別。
  八個元件 metadata provider 與相關測試同步遷移，錯誤碼、文字、Error payload 與
  跨程序格式不變。`common.error` 加入拒絕錯誤 enum 及對調 table 的編譯期斷言。
  這是 catalog 建置 API 的變更，不提供可混用的舊 entry 別名。
- **已接線：Module 契約故障的獨立共享紀錄。** Common 的 fatal 入口不再先寫 stderr。
  使用者後續確認交付可靠性優先於回報延遲；已放棄程序內 mailbox＋watchdog 20ms 掃描提案。
  ModuleSystem 在 spawn 前建立固定共享紀錄，Module 在使用者工廠／hooks 前綁定。
  故障路徑不配置、不取得 Socket／stdio mutex，提交後 `_Exit`；中央於確認 OS 退出後讀取。
  不增加背景執行緒，不以 500ms 作為 fatal 紀錄交付期限。後續中央 fatal 監督使用既有
  ModuleManager 迴圈觀察共享原子狀態，不是先前的程序內 mailbox 提案。
  身分、索引、偵測位置、格式與生命週期見
  [ModuleSystem 契約故障](../module_system/README.md#契約故障共享紀錄)。

#### 本輪建置與驗證

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 完整專案建置通過，包含 SDK 個別公開標頭編譯。
兩種組態的錯誤介面相關 CTest 各 **12/12** 通過（Debug 41.38 秒、Release 42.39 秒），
涵蓋 Common 四套、OSAL 三套、Concurrency／Message Storage 各一套、Message Bus 兩套
與 Module 錯誤協定一套。Catalog fixture 實際驗證 clean-first、來源缺檔恢復與變更
provider symbol 後的增量查詢；容量 fixture 重新建置深度 2／5，並拒絕深度 1。
已核對 Ninja 對生成來源具有真正的 copy 指令，不再是空命令。
以上是 catalog／table 修改完成時的驗證；後續致命回報接線的驗證另列於本頁末段。
Linux／macOS 尚未原生驗證。


## Shared time type

`<vinc/time.hpp>` provides `vinc::Deadline`, an alias for
`std::chrono::steady_clock::time_point`. Link `vinc::common` to obtain its include path;
the header itself depends only on `<chrono>` and adds no runtime service.

A deadline is an absolute cutoff for a local wait. Passing the same value through
several steps preserves a total waiting budget. It is not a duration or wall-clock
timestamp; do not serialize its representation or epoch as an IPC/network contract.
Individual APIs continue to define timeout, cancellation and infinite-wait behavior.

OSAL, Control Channel and their callers use this common type. Control Channel retains
its existing `Clock` alias for obtaining `steady_clock` times, but no longer defines
a separate Deadline alias. OSAL's former error bridge header has been removed:
include `<vinc/result.hpp>` for common errors/results and `<vinc/time.hpp>` for deadlines.
The domain migration later introduced a new `<vinc/osal/error.hpp>` owning OSAL enums;
it is not the former common-type forwarding header.

Verified on 2026-10-04 with the Windows x64 toolchain listed above: Debug/Release
builds and public-header compilation succeeded (including `time.hpp` as the first
include), with **28/28** CTest suites passing in each configuration. The standalone
OSAL Release build passed **9/9** suites. A token comparison of 54 existing C++
files confirmed changes were limited to includes, Deadline qualification/alias
removal and formatting. Linux/macOS were not built or run in this round.

## Temporary system settings

`<vinc/config.hpp>` provides the `vinc::config` namespace as a temporary home for
centralized system settings. Runtime defaults will be added gradually within
explicitly authorized work and will later be managed by Param. Layout settings
such as `error_chain_max_depth` remain compile-time only; its contract is described
under bounded cause chains above.

`module_fatal_exit_timeout` defaults to five seconds after central first observes a
fatal state. Earlier startup or explicit-stop deadlines win. This is a termination
request policy, not a hard OS exit guarantee; see the [ModuleSystem contract](../module_system/README.md#fatal-退出監督2026-10-06).

The header currently contains Message Bus defaults (ring buffer slots, message byte budget,
topic/participant/shared-memory budgets), control request/queue bounds, the request
timeout, and reader-wait retry settings. Their values are unchanged from the original
Message Bus implementation.

OutputHub settings are also centralized here: queue/capture defaults, admission limits,
read/completion budgets, poll/failure/shutdown intervals, and text presentation limits.
Their original values are preserved. `Limits` and `QueueLimits` still accept caller
overrides; fixed stream indices and UTF-8 encoding constants stay in OutputHub.

Local Shell settings now include line/argument/history/job limits, output batch sizes,
command/completion deadlines, terminal polling, headless backoff, and shutdown
collection/reporting intervals. Their values are unchanged; shutdown polling does
not impose an overall shutdown timeout. Session identities and terminal display
constants remain in Shell.

Socket Shell settings include listener defaults, connection/session bounds, network
queue budgets, output subscription limits, polling and final drain deadlines. The
poller capacity derives from the maximum live clients plus its listener; script
mailbox capacity covers retained remote owners plus the local owner. Tcl output,
delivery/result bounds and cancellable sleep slices are also centralized. Existing
values and `SocketShellOptions` overrides are preserved; recovery-token dimensions
and wire-format constants stay in Shell.

Protocol/layout constants remain in the owning component; test workloads remain in
tests. These are compile-time settings, not dynamic updates, persistence, or parameter
registration. Publisher options and BusManager limits can still be supplied through their
existing APIs.

Module 一般 Socket 故障回報等待使用 `module_error_report_timeout`（500ms），不覆蓋停止期限。
契約故障改用共享紀錄，不使用這個 Socket 等待期限。

ModuleSystem migration verification (2026-10-04): Windows x64 Debug/Release full
builds and 33/33 CTest suites passed in each configuration. The added Module,
Control Channel and Message Bus metadata providers share the selected static catalog;
`module.error_protocol` verifies numeric cause-chain transport and local truncation.
These results do not establish Linux/macOS support or reliable delivery after a crash.

## Contract violations

`<vinc/contract.hpp>` provides `[[noreturn]] vinc::contract_violation(reason, location)`
and `contract_index_violation(reason, index, size, location)`. Both are fatal in
Debug and Release. Neither constructs a framework exception, formats text, allocates
owned strings, nor writes synchronously to stderr before termination.

Common remains standard-library-only. Its internal `detail/runtime_services.hpp`
defines one framework bootstrap binding per executable/shared-library image, not a
public handler registry. It replaces the former ContractReporter. ModuleSystem
binds before application construction and clears after runtime services finish.
Normal successful accesses do not read the binding or increment the reader counter.
Only fatal calls use these lock-free local atomics. There is no added Common worker.

With the Module runtime binding, a violation publishes a bounded shared record and
exits the child without invoking user hooks/destructors. With no binding (including
the central process and standalone Common consumers), or when the callback returns,
the fallback is now `std::_Exit(77)`. User terminate handlers, automatic/static
destructors, atexit handlers and stdio flushes are bypassed. This approved change
applies only to framework contract violations, not ordinary C++ exception handling.
No hard OS exit latency is promised. This is not a signal handler or crash catcher.

Use these entry points only for broken programming contracts, such as invalid unchecked
indices or expired message access. Recoverable runtime failures continue to use Result.
Reason pointers must refer to readable, terminated text through the call. Source locations
identify the framework detection site, not a captured user stack trace. Standard allocation,
third-party and user-defined exceptions retain their existing boundaries.

本輪 Module 致命回報、中央保留與 Shell 顯示的驗證，集中於
[ModuleSystem 驗證](../module_system/README.md#共享故障紀錄驗證)，不把原先 catalog
測試結果視為跨程序致命回報的證據。

## RuntimeServices bridge（2026-10-06）

`detail::RuntimeServices` is a Host-owned immutable service table containing a
version, byte size, opaque context pointer and one `report_contract` callback.
Version 1 supports only contract fatal reporting. Message Bus, Param and other
ordinary Module services still belong to the future Context; no registry or
automatic service discovery is introduced.

The callback takes a borrowed `ContractViolation`: reason/file/function pointers,
line, has-index flag and bounds. `contract.cpp` flattens source_location before
the call, so no STL ownership or source_location representation crosses the DLL
boundary. Pointers are valid only in the same process and for the duration of the
call. This requires matching architecture/toolchain/build contracts; the version
and size checks do not establish cross-compiler compatibility or a stable Plugin ABI.

### Binding and lifetime

- `bind_runtime_services()` returns `Result<void>`. It rejects unsupported versions,
  undersized tables and missing callbacks without consuming the binding. Caller must
  supply readable storage for a real RuntimeServices object; this is not a parser
  for untrusted bytes. A same-version larger table can contain an unused tail.
- Each image allows one successful binding during its lifetime. Repeated binding,
  including after clear, is rejected; replacement/hot unload is outside this round.
- Initialization/clear are serialized by the framework, outside DLL loader hooks.
  Bind before application work. Host table, callback code and context remain valid
  until every borrowing image has cleared its binding and any callbacks have finished.
- Stop/join Plugin work first. `clear_runtime_services()` withdraws the local pointer
  and waits for existing fatal readers, preserving the former reporter's lifetime
  protection. It does not cancel callbacks or free their state. New fatal calls see
  null and take direct fallback. A stuck callback can prevent clear from returning;
  ModuleSystem's existing central supervision remains responsible for termination.
- Loading-time static constructors can run before injection. They use fallback and
  have no guarantee of a shared report. Post-clear faults also use fallback.
- Executable and DLL each link their own static Common bridge copy; only the binding
  and reader count are local. Both refer to the same Host-owned services. `VINC_LOCAL`
  makes bridge/fatal entry symbols hidden on GCC/Clang to prevent ELF interposition;
  these symbols are not DLL exports on Windows. Common is compiled as PIC for shared
  library consumers. Production Plugin build rules now follow the Module System contract.

### Error metadata and verification

`ErrorDomain::Common = 9` is appended without renumbering prior domains.
`common_error.hpp` supplies typed invalid-table, incompatible-version and
already-bound errors. `vinc_common_errors` registers a header-only static catalog;
Common runtime does not depend on the generated catalog or ModuleSystem.
Fatal dispatch itself never formats an Error or allocates a diagnostic string.

`common.runtime_services` checks validation, duplicate binding, no rebind after
clear, unbound/cleared/returning-callback fallback, and bypass of terminate/atexit/
destructor sentinels. Fatal probes also disable ordinary C++ allocation; the bound
probe validates context, bounds and source location without allocating.
Actual two-DLL tests and platform verification are recorded in
[ModuleSystem](../module_system/README.md#runtimeservices-bridge-接線2026-10-06).

### Production Plugin integration

The Module System now uses this bridge in its production ModuleHost loader.
Initialization happens after native loading and before Module creation. Plugin
clear follows Module destruction and Host worker shutdown, before the Host table
or report storage expires. This does not expand RuntimeServices beyond fatal reporting.
See [Module System](../module_system/README.md) for the matching-build ABI and loader limits.

## 共享資源名稱接線

`detail/resource_names.hpp` 只定義 `ResourceNames::reserve()`／`retire()` 抽象，使用標準型別與 Result。
它讓 Bus 接受中央提供的名稱服務，而不反向依賴 Module System 或檔案系統。
這不是 runtime error registry，也不是使用者可取得框架共享區的公開入口。
資源建立前記錄、名稱退役與上限由 [Module System](../module_system/INSTANCE.md) 維護；
`config.hpp` 的 `instance_*`、`module_lock_*` 是本輪新增的暫時系統設定。

## Param settings and catalog (2026-10-08)

`ErrorDomain::Param = 10` identifies the component-owned static catalog. Common
does not depend on Param runtime. New `param_*` constants in config bound names,
strings, snapshots, file input and pending work; their defaults and limits are
maintained in the [Param README](../param/README.md). Existing framework settings
remain in config; adding Param does not migrate them or enable dynamic rebinding.


## Log 設定與錯誤分類（2026-10-08）

新增 ErrorDomain::Log = 11，錯誤／操作 enum 與靜態 catalog 由 [Log](../log/README.md) 維護。
config 的 log_* 預算與間隔僅服務新功能；本輪未把既有設定遷入 Param。

## Bag 設定與錯誤（2026-10-12）

新增 ErrorDomain::Bag=12、NativeErrorSource::Sqlite=11。文字表由 Bag 元件註冊，
common 不依賴 SQLite。config::bag_* 保存 queue／單筆預算、主資料庫大小上限、批次、探索／pump 間隔、
metadata／查詢限制與 Recorder 收尾期限；定義和限制見 [Bag README](../bag/README.md)。

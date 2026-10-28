<a id="common-framework-interfaces"></a>

# Common framework interfaces / 框架共通介面

The public entry points are `error.hpp` and `result.hpp`. The unused legacy integer error table was removed after the Module System migration.

正式入口為 `error.hpp`／`result.hpp`。ModuleSystem 遷移後已移除無呼叫端的舊整數錯誤表。

<a id="shared-owner-identities"></a>

## Shared owner identities / 共享持有者身分

`<vinc/owner_id.hpp>` defines the standard-library-only numeric boundary shared by
ModuleSystem and Concurrency: zero is unowned, 1 through 2^63−2 are Module owners,
and 2^63−1 is the central owner. ModuleSystem exposes its MID range in `mid.hpp`
and checks the allocator there; Concurrency validates without truncating values.
This is a fixed protocol boundary, not a runtime Param setting. One bit of the
64-bit shared control word remains available for atomic publication and release.
No registry or dependency on ModuleSystem is introduced in common.

`<vinc/owner_id.hpp>` 定義 Module System 與 Concurrency 共用的純標準庫數值邊界：0 為無持有者，1～2^63−2 為 Module，2^63−1 為中央。Module System 在 `mid.hpp` 公開 MID 範圍並檢查配置器；Concurrency 驗證時不截斷數值。這是固定協定邊界，不是執行期 Param。64-bit 控制字仍留一個位元供原子發布與解鎖。Common 不新增 registry 或 Module System 相依。

`config::concurrency_retry_interval` defaults to 5 ms. It controls cancellable
shared-mutex retry sleeps, not a maximum holding time; see the
[Concurrency contract](../components/concurrency/README.md).

`config::concurrency_retry_interval` 預設 5ms，控制可取消的共享 mutex 重試休眠，不是持鎖期限；詳見 [Concurrency 契約](../components/concurrency/README.md)。

<a id="error-interface"></a>

## Error interface / 錯誤介面

`vinc::Result<T>` is `std::expected<T, vinc::Error>`. Success is its value state;
`Error{}` is an unknown failure, never success. Common owns `ErrorDomain`, the
value-only payload, typed construction/matching, catalog contracts and formatting
helpers. Each component owns its own `int32_t` error and operation enums.

`vinc::Result<T>` 即 `std::expected<T, vinc::Error>`，value 狀態表示成功；`Error{}` 永遠是未知失敗。Common 擁有 `ErrorDomain`、純值 payload、型別安全建構／比較、catalog 契約與格式化 helper；各元件自行維護底層為 `int32_t` 的錯誤與操作 enum。

`ErrorDomain::Ui = 13` is reserved for the optional official Web UI Module.
Its static catalog and `ui_*` configuration defaults belong to the
[UI component contract](../ui/README.md); common does not depend on HTTP or Vue.

`ErrorDomain::Ui = 13` 用於可選啟動的官方 Web UI Module。靜態 catalog 與 `ui_*` 預設值由 [UI 契約](../ui/README.md) 維護；Common 不依賴 HTTP 或 Vue。

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

`Error` 包含目前層的 `domain`、有號 `code`／`operation`、`NativeError` 及有界數值原因陣列。使用元件 enum 搭配 `vinc::make_error()` 或型別安全建構子；跨 domain 混用 code 與 operation 會在編譯期拒絕。`is()`／`is_operation()` 比較包含 domain。原始欄位及 `Error::from_raw()` 供已驗證的傳輸轉接器使用，也可保留未知值；複製不配置、不依賴 category 物件生命週期。

The sole text entry point is `std::string vinc::error_to_string(const Error&)`.
It may allocate/throw; do not parse its human-readable text as a protocol. Unknown
domains, codes, operations and native sources remain numeric. Domain zero is
unknown; operation zero omits operation context. Registered error codes are nonnegative;
registered operations are positive. Both use explicit stable assignments. Code zero
may identify a legacy failure and does not mean success. Never renumber/reuse published identifiers.

唯一文字入口為 `std::string vinc::error_to_string(const Error&)`，可能配置／拋例外；不得將診斷文字解析成協定。未知 domain、code、operation、native source 保留數值。domain 0 為未知，operation 0 不提供操作資訊；登記錯誤碼為非負數、操作碼為正數，皆明確穩定編號。code 0 可表示既有失敗，不能當成成功；已發布編號不得重編或重用。

<a id="bounded-cause-chains"></a>

### Bounded cause chains / 有界原因鏈

`config::error_chain_max_depth` in `<vinc/config.hpp>` defaults to **3**, counting
the current error plus two causes. Its minimum is **2**. It is a compile-time
layout setting, not a runtime Param: rebuild every linked C++ consumer with the
same value after changing it. The Error layout remains standard-layout and
trivially copyable, including for existing Windows native I/O containers; neither
property makes it a wire format or a stable binary ABI.

`<vinc/config.hpp>` 的 `config::error_chain_max_depth` 預設 **3**，含目前層與兩層原因，最小 **2**。這是影響布局的編譯期設定，不是執行期 Param；修改後所有連結的 C++ 使用端須用同值重建。Error 維持 standard-layout 與 trivially copyable，供 Windows 原生 I/O 容器使用；這些性質不代表 wire format 或穩定 ABI。

At an unrecoverable component boundary, wrap the lower error with the current
component's code and operation:

跨越無法復原的元件邊界時，使用本層錯誤碼與操作包裝下層失敗：

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

- `wrap_error()` 加入目前層並繼承下層原生診斷；建構、複製、包裝均不配置、不持有 pointer。`std::unexpected(existing_error)` 原樣轉交。
- `record()` 回傳目前數值紀錄副本；`causes()` 回傳由近到深的唯讀 span，借用原 Error，銷毀／變更後失效，拒絕從暫時 Error 取用。`cause_storage` 僅為維持 standard-layout 而可見，使用端須用 accessor。
- `is()`／`is_operation()` 只檢查目前層；相等比較包含完整已存原因鏈與截斷狀態。
- 滿載時保留新目前層、容得下的最近原因與最深原因。容量 3 時，將 `C <- B <- A` 包進 D 會得到 `D <- C <- [intermediate causes omitted] <- A`；容量 2 只保留目前與最深層，再次包裝仍保留 `truncated()`。
- `error_to_string()` 逐筆查靜態 catalog，於最深原因前標記省略；可能配置／拋例外，單筆格式不變。
- 已驗證轉接器可將目前數值欄位、近因優先 span 及省略旗標傳入 `Error::from_raw()`。標示截斷時至少須有最深原因，已省略部分必須位於它之前。超長輸入依同樣政策裁減；此 factory 不負責協定驗證。

The chain describes one failure's propagation. Independent cleanup failures are
not automatically causes of the original failure; their reporting policy requires
a separate component decision.

原因鏈描述同一失敗的傳播。獨立的收尾失敗不會自動變成主要失敗的原因，回報政策由元件另定。

Windows x64 MSVC measurements (bytes; Debug and Release agree at default capacity):

Windows x64／MSVC 實測大小，單位 bytes；預設容量下 Debug／Release 相同：

| Configuration | `Error` | `Result<void>` | `Result<int>` |
| --- | ---: | ---: | ---: |
| Before cause chains | 32 | 40 | 40 |
| Capacity 2 | 80 | 88 | 88 |
| Default capacity 3 | 112 | 120 | 120 |
| Capacity 5 | 176 | 184 | 184 |

| 組態 | `Error` | `Result<void>` | `Result<int>` |
| --- | ---: | ---: | ---: |
| 加入原因鏈前 | 32 | 40 | 40 |
| 容量 2 | 80 | 88 | 88 |
| 預設容量 3 | 112 | 120 | 120 |
| 容量 5 | 176 | 184 | 184 |

The fixed diagnostic storage increases Result size even on the success path.
These are measured object sizes, not throughput measurements or cross-platform
size guarantees. Capacity 2/5 tests rebuild isolated copies of common; capacity 1
must fail the minimum-capacity assertion.

固定診斷儲存也增加成功路徑的 Result 大小。以上是物件大小量測，不是吞吐量或跨平台大小保證。容量 2／5 測試會重建獨立 Common 副本；容量 1 必須觸發最小容量斷言。

<a id="component-catalogs-and-cmake-composition"></a>

### Component catalogs and CMake composition / 元件 catalog 與 CMake 組裝

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

`cmake/VincMsvcDependencies.cmake` 是 Common 引入的純建置 helper。Windows MSVC／Ninja 下，明確以 UTF-8 探測本地化 `/showIncludes` 並核對已知 Unicode 標頭名，確認後才接受前綴。延後更新在最上層 CMake 目錄、編譯器快取重載後執行，外部 `add_subdirectory` 使用端也適用。無法驗證編碼時，以探測 log 拒絕配置，不默默採用；需 UTF-8 開發環境。其他編譯器／產生器不變，無新增 runtime 相依。既有遺失標頭相依的物件須 clean rebuild 一次；重現、測試與限制見 [SDK 建置紀錄](../sdk/README.md#msvcninja-增量建置修復2026-10-06)。

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

- `include/vinc/error_catalog.hpp` 定義不可變 catalog 與 constexpr 驗證。元件 `error.hpp` 維護 enum／traits，`detail/error_catalog.hpp` 維護文字與本地表格。
- `cmake/VincErrors.cmake` 每元件登記一次 provider target、literal include 路徑與完整 C++ symbol；CMake 不解析 enum 或維護逐筆文字。
- Provider 只公開錯誤 metadata 與可選原生格式化程式，僅依賴 Common，不依賴元件 runtime 或 `vinc::errors`。
- `vinc_finalize_error_catalog()` 凍結登記，在建置目錄保存 configure-time 輸入；Common 自有 build rule 產生 C++ translation unit，clean／刪除後也可重建。須在所有 provider 之後恰好呼叫一次，生成表以 `static_assert` 驗證。
- `vinc::common` 不反向依賴元件或 `vinc::errors`。後者提供 formatter 定義，依賴 Common 與 metadata provider。每次建置選一份 catalog，每個最終程式只有一份 formatter；不用 runtime registry、可變 map 或靜態登記建構子。

Example provider registration (declarations/descriptions stay in C++):

Provider 登記範例，宣告與文字仍由 C++ 維護：

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

僅用 Common 且需文字查詢時，連結 `vinc::common` 與 `vinc::errors`。`vinc::osal` 與 SDK 會傳遞兩者。獨立 OSAL 自帶最小 catalog，只需相鄰 Common，不引入其他 runtime；獨立 Common 可用空 catalog，以數值 fallback。自組 Common／OSAL 的父專案須於 provider 登記後 finalize；完整框架在 `src/vinc/CMakeLists.txt` 完成。太晚／重複登記或缺少／重複 finalize 均為配置錯誤。

C++ validation rejects duplicate domain/code/operation entries, mismatched domains and empty descriptions. C++23 cannot enumerate arbitrary enum members; component tests must check missing entries. OSAL currently has contiguous assignments for 23 error codes and 48 nonzero operations. Future sparse catalogs require appropriate coverage checks.

C++ 驗證會拒絕重複 domain／code／operation、不符 domain 與空文字。C++23 無法列舉任意 enum 全部成員，漏表覆蓋須由元件測試負責。OSAL 現有範圍為連續 23 個錯誤碼與 48 個非零操作碼；未來稀疏 catalog 需提供適用的覆蓋檢查。

<a id="native-identity-and-process-boundaries"></a>

### Native identity and process boundaries / 原生身分與跨程序邊界

`NativeError` stores a source namespace and an `int64_t` value, including negative
resolver codes and unsigned 32-bit Win32/NTSTATUS values. It contains no
`std::error_code` or process-local pointer. Native namespaces distinguish Win32,
Winsock, NTSTATUS, Linux/Darwin errno/resolver and platform generic-category values.
The current POSIX numeric namespaces target the project's x86-64 native ABI;
additional architectures require validation of the numeric namespace before reuse.

`NativeError` 保存來源命名空間與 `int64_t`，可包含負 resolver code 及 unsigned 32-bit Win32／NTSTATUS 值，不含 `std::error_code` 或程序本地 pointer。來源區分 Win32、Winsock、NTSTATUS、Linux／Darwin errno／resolver 與平台 generic category。目前 POSIX 數值命名空間以專案 x86-64 原生 ABI 為準，其他架構須先驗證再沿用。

The OSAL metadata provider formats supported host-native sources. Foreign or
unsupported sources keep their number; NTSTATUS currently uses numeric fallback
and is never interpreted as a Win32 code. Standard thread-creation exceptions
are converted at the existing boundary. Unrecognized std::error_category values
have no portable category identity and retain only the number under unknown source
UINT32_MAX; arbitrary third-party categories need an explicit stable adapter.

OSAL metadata provider 格式化支援的本機來源。外來／未支援來源保留數值；NTSTATUS 目前數值 fallback，不冒充 Win32。標準執行緒建立例外在既有邊界轉換；未知 `std::error_category` 無可攜身分，只以來源 UINT32_MAX 保留數值。第三方 category 須另有穩定 adapter。

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

不得 memcpy Error／Result 到 IPC frame。各傳輸須定義版本、位寬、位元組序、封裝驗證與未知值政策。`osal.error_process` 示範有界、測試專用的共享記憶體欄位編碼；fixture v2 含筆數、省略旗標及數值紀錄，重建前驗證版本／旗標／筆數／長度。子程序退出後父程序才查詢文字，涵蓋未知巢狀原因、截斷與合成超長發送端。這不是正式 codec；該次測試不修改 Module／Bus 正式格式，也不承諾跨版本 C++ ABI。

<a id="layer-translation-and-migration-status"></a>

### Layer translation and migration status / 分層轉換與遷移現況

Each layer first attempts its permitted recovery; an unrecoverable lower-layer
failure is translated into that layer's public code/operation. Callers need not
interpret lower-layer codes. Use `wrap_error()` to preserve bounded causes and native
provenance; do not silently turn all failures into one generic code.

各層先嘗試權限內的復原；無法復原時轉成本層公開 code／operation，呼叫端不必理解下層分類。以 `wrap_error()` 保留有界原因與原生來源，不能把所有失敗默默抹成同一通用碼。

- OSAL uses common Result/Error and owns 23 error codes and 48 nonzero operations; see the [OSAL contract](../osal/README.md#統一錯誤介面).
- OutputHub wraps OSAL capture/spawn failures in its own code/operation and retains diagnostic causes.
- Control Channel and Module System use common Result/Error, static catalogs and bounded wrapping. The Module error envelope explicitly transports numeric causes; its version is distinct from the bootstrap marker. Hooks, Manager creation and adapters use the same error contract.
- Message Bus uses common errors for registration/publication, with bounded Storage/OSAL/Control causes. Bus v2 control responses encode numeric chains; legacy payload/adapters are removed. See its README/DESIGN for version rejection and validation.
- Message Storage retains CapacityExceeded=0; code zero can mean failure, operation zero means unspecified, and only Result's value state means success. Internal expected failures use Result, allocation boundaries retain bad_alloc/length_error conversion, checked APIs return Result, invalid unchecked access is fatal, and AccessError is removed.
- Concurrency translates SharedMemory failures into its own codes with OSAL causes; local failures remain single-layer.
- Shell factories, parsing, routing, socket startup, terminal/service failures and script launch use common errors with Module System/OSAL/OutputHub causes. Script completion separates Result from bounded Tcl text; its catalog has no Tcl runtime dependency. See Shell for application statuses and Tcl return options.

- OSAL 已使用共同 Result／Error，維護 23 個錯誤碼及 48 個非零操作碼；外層操作與內部原因見 [OSAL 契約](../osal/README.md#統一錯誤介面)。
- OutputHub 維護自己的小型 catalog，將 OSAL 擷取／spawn 失敗轉成本層 code／operation，保留原生原因。
- Control Channel／Module System 使用共同 Result／Error、元件 catalog 與有界包裝。Module 錯誤封裝明確編碼數值原因鏈；不是以 bootstrap marker 當作 error wire 版本。Hooks、Manager 建立與便利轉接採同一契約。
- Message Bus 在註冊／發布使用共同 Result／Error、靜態 catalog，以及有界 Storage／OSAL／Control 原因。Bus v2 控制回覆傳數值原因鏈，舊 payload／adapter 已移除；版本拒絕與驗證見該 README／DESIGN。
- Message Storage 包含既有 CapacityExceeded=0；catalog 允許錯誤碼 0，operation 0 仍為未指定，成功只由 Result value 表示。內部預期失敗直接傳 Result，配置邊界保留 bad_alloc／length_error 轉換。Checked API 回 Result，非法 unchecked 存取走契約終止；AccessError 已移除。
- Concurrency 使用共同 Result／Error 與靜態 provider；SharedMemory 失敗包成本層碼，保留 OSAL 原因，本地失敗不額外加層。
- Shell 的 factory、解析、路由、Socket 啟動、終端／服務及腳本啟動皆使用共同 Result／Error，保留 Module System／OSAL／OutputHub 原因。腳本完成將 Result 與有界 Tcl 文字分開；靜態 catalog 不依賴 Tcl runtime。應用狀態碼與 Tcl return options 見 Shell README。

<a id="error-catalog-migration-verification"></a>

### Error catalog migration verification / 錯誤 catalog 遷移驗證紀錄

2026-10-04，Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：

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

- Debug／Release 完整建置與公開標頭編譯通過，全套 CTest 各 **30/30**。
- 乾淨獨立 OSAL Release 建置與 **11/11**；無 OSAL provider 的乾淨 Common Release 建置與 **2/2** 通過。
- `common.error_catalog` 建置／執行有效隔離使用端，檢查七種故意失敗：重複 code／domain、domain 不符、重複／太晚登記、缺少／重複 finalize，均須有預期診斷。
- `osal.error_process` 真實子程序驗證明確編碼、原生與未知值、退出後查詢、截斷與非法版本。
- OutputHub 驗證 spawn 失敗的外層 domain／code 及原生來源；原有程序、控制與 Bus 測試通過。
- NTSTATUS 用合成值，未注入真正 BCryptGenRandom 失敗。Linux／macOS 來源有更新，該次未在原生環境建置或執行。

Build-time catalog pointers and native formatter callbacks remain process-local.
The passing fixture does not establish a production cross-version error wire ABI.

Catalog pointer 與原生 formatter callback 只在本程序有效；fixture 通過不代表正式跨版本錯誤 wire ABI。

<a id="cause-chain-verification-2026-10-04"></a>

### Cause-chain verification (2026-10-04) / 原因鏈驗證紀錄（2026-10-04）

Using the Windows x64 toolchain listed above:

使用上述 Windows x64 工具鏈：

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

- Debug／Release 完整建置與 SDK 公開標頭編譯通過；Debug 全套 **32/32**。
- Release 初次 **31/32**，catalog consumer 沿用舊 Error 布局物件；log 顯示只有 formatter 重建，使用端物件時間未變。隔離 catalog／容量 fixture 改為 clean→configure→build，確保布局一致並重建來源。最後兩組態 Common 各 **4/4**，含原先失敗案例；各 32 項均曾通過，Release 包含補跑而非單次全套成功。
- 乾淨獨立 OSAL Release 含 Common **13/13**。
- 原因測試涵蓋型別包裝、原樣轉交、有界順序、最深原因、重複截斷、獨立副本、未知值與文字；容量 2／5 獨立重建，容量 1 須出現指定斷言。
- 真實子程序退出後檢查巢狀／截斷、畸形封裝及合成較大容量發送端。
- OutputHub failed-spawn 同時查外層與 OSAL 原因；capture-read 採相同 wrapper，但未注入原生讀取失敗。
- 該次未建置／執行 Linux／macOS，也未改正式 IPC 或當時 ModuleManager 的錯誤契約。

<a id="待處理事項"></a>

## Resolved review findings and follow-up references / 已解決審查事項與後續參照

<a id="2026-10-05-common-重審修正"></a>

### Common review fixes (2026-10-05) / 2026-10-05 Common 重審修正

- **Fixed catalog regeneration after clean.** Cross-directory `file(GENERATE)` previously left Ninja an empty generation command. The finalizer now preserves input with `file(CONFIGURE)`, while Common owns the build-time generator and consumer target. Tests cover clean-first, deleted-source recovery, changed provider symbols without clean, and the original seven negative cases. No runtime registry/dependency was added.
- **Fixed interchangeable code/operation entries.** `ErrorCodeEntry` and `ErrorOperationEntry` replace `ErrorEntry`, with distinct enum traits and span types. Eight metadata providers and tests migrated without changing numeric codes, text, payload, or IPC. Compile-time tests reject swapped enums/tables; the old interchangeable entry alias is not retained.
- **Wired an independent shared contract-fault record.** Fatal dispatch no longer writes stderr first. The in-process mailbox/watchdog-20 ms proposal was abandoned. Module System allocates a fixed record before spawn and binds before factories/hooks; the fatal path allocates nothing, takes no Socket/stdio mutex, publishes then `_Exit`s. Central reads after confirmed exit and also observes atomic state from its existing supervision loop. No worker was added; the 500 ms Socket timeout is not a fatal-record delivery deadline. See [Module faults](../module_system/README.md#契約故障共享紀錄).

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

<a id="本輪建置與驗證"></a>

#### Build and validation for that review / 本輪建置與驗證

Windows x64／MSVC 19.51.36257、SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2: full Debug/Release builds and SDK header compilation passed. The error-related suites passed **12/12** each (41.38 s / 42.39 s): four Common, three OSAL, one each Concurrency/Storage, two Bus, one Module error protocol. Fixtures verify clean-first, missing-source regeneration, provider-symbol incremental lookup, capacities 2/5 and rejection of 1; Ninja has a real copy command. These are catalog/table results, not later fatal-report evidence. Linux/macOS were not natively verified.

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 完整專案建置通過，包含 SDK 個別公開標頭編譯。
兩種組態的錯誤介面相關 CTest 各 **12/12** 通過（Debug 41.38 秒、Release 42.39 秒），
涵蓋 Common 四套、OSAL 三套、Concurrency／Message Storage 各一套、Message Bus 兩套
與 Module 錯誤協定一套。Catalog fixture 實際驗證 clean-first、來源缺檔恢復與變更
provider symbol 後的增量查詢；容量 fixture 重新建置深度 2／5，並拒絕深度 1。
已核對 Ninja 對生成來源具有真正的 copy 指令，不再是空命令。
以上是 catalog／table 修改完成時的驗證；後續致命回報接線的驗證另列於本頁末段。
目前各平台的建置與測試範圍見[平台驗證報告](../../../README.md#validation-and-limits)。

<a id="shared-time-type"></a>

## Shared time type / 共用時間型別

`<vinc/time.hpp>` provides `vinc::Deadline`, an alias for
`std::chrono::steady_clock::time_point`. Link `vinc::common` to obtain its include path;
the header also exposes the Unix/UTC utilities below. Common still depends only on the standard library
and adds no runtime service or background thread.

`<vinc/time.hpp>` 的 `vinc::Deadline` 是 `std::chrono::steady_clock::time_point`。連結 `vinc::common` 取得 include 路徑；同標頭也提供下述 Unix／UTC 工具。Common 仍只依賴標準庫，不新增服務或背景執行緒。

A deadline is an absolute cutoff for a local wait. Passing the same value through
several steps preserves a total waiting budget. It is not a duration or wall-clock
timestamp; do not serialize its representation or epoch as an IPC/network contract.
Individual APIs continue to define timeout, cancellation and infinite-wait behavior.

Deadline 是本地等待的絕對截止點；跨步驟傳同一值可保持總等待預算，不是 duration 或日曆 timestamp，不得把其表示法／epoch 當 IPC／網路協定。各 API 仍自行定義逾時、取消與無限等待。

OSAL, Control Channel and their callers use this common type. Control Channel retains
its existing `Clock` alias for obtaining `steady_clock` times, but no longer defines
a separate Deadline alias. OSAL's former error bridge header has been removed:
include `<vinc/result.hpp>` for common errors/results and `<vinc/time.hpp>` for deadlines.
The domain migration later introduced a new `<vinc/osal/error.hpp>` owning OSAL enums;
it is not the former common-type forwarding header.

OSAL、Control Channel 與呼叫端共用此型別。Control 保留取得 steady_clock 的 `Clock` alias，不再另定 Deadline。OSAL 舊轉接頭檔已移除；以 `<vinc/result.hpp>` 取錯誤／結果、`<vinc/time.hpp>` 取期限。後來 domain 遷移新增的 `<vinc/osal/error.hpp>` 擁有 OSAL enum，不是舊轉接頭檔。

Verified on 2026-10-04 with the Windows x64 toolchain listed above: Debug/Release
builds and public-header compilation succeeded (including `time.hpp` as the first
include), with **28/28** CTest suites passing in each configuration. The standalone
OSAL Release build passed **9/9** suites. A token comparison of 54 existing C++
files confirmed changes were limited to includes, Deadline qualification/alias
removal and formatting. Linux/macOS were not built or run in this round.

2026-10-04，以上述 Windows x64 工具鏈驗證 Debug／Release 建置與公開標頭（含 time.hpp 作第一個 include）通過，全套各 **28/28**；獨立 OSAL Release **9/9**。54 份既有 C++ 的 token 比較確認僅 include、Deadline qualification／alias 移除及排版；該次未建置／執行 Linux／macOS。

<a id="temporary-system-settings"></a>

## Temporary system settings / 暫時系統設定

`<vinc/config.hpp>` provides the `vinc::config` namespace as a temporary home for
centralized system settings. Runtime defaults will be added gradually within
explicitly authorized work and will later be managed by Param. Layout settings
such as `error_chain_max_depth` remain compile-time only; its contract is described
under bounded cause chains above.

`<vinc/config.hpp>` 的 `vinc::config` 暫時集中系統設定。執行期預設依授權逐步加入，未來交由 Param；如 `error_chain_max_depth` 的布局設定維持編譯期，契約見有界原因鏈。

`python_traceback_bytes` bounds the diagnostic emitted for one escaped Python hook
exception (32 KiB by default). Python lifecycle and error conversion contracts are
maintained in [Python Module](../module_system/python/README.md).

`python_traceback_bytes` 限制單次逃出 Python hook 例外的診斷，預設 32 KiB。生命週期與轉換契約見 [Python Module](../module_system/python/README.md)。

`module_fatal_exit_timeout` defaults to five seconds after central first observes a
fatal state. Earlier startup or explicit-stop deadlines win. This is a termination
request policy, not a hard OS exit guarantee; see the [ModuleSystem contract](../module_system/README.md#fatal-退出監督2026-10-06).

`module_fatal_exit_timeout` 預設為中央首次觀察 fatal 後 5 秒，較早的啟動／明確停止期限優先。這是要求終止的政策，不是 OS 硬性退出保證；見 [Module 契約](../module_system/README.md#fatal-退出監督2026-10-06)。

The header currently contains Message Bus defaults (ring buffer slots, message byte budget,
topic/participant/shared-memory budgets), control request/queue bounds, the request
timeout, and reader-wait retry settings. Their values are unchanged from the original
Message Bus implementation.

標頭包含 Bus 槽位、訊息 byte、topic／參與者／共享記憶體預算、控制請求／佇列、請求逾時及 Reader 等待重試設定；集中設定時維持原有值。

OutputHub settings are also centralized here: queue/capture defaults, admission limits,
read/completion budgets, poll/failure/shutdown intervals, and text presentation limits.
Their original values are preserved. `Limits` and `QueueLimits` still accept caller
overrides; fixed stream indices and UTF-8 encoding constants stay in OutputHub.

OutputHub 的佇列／擷取預設、接收上限、讀取／完成預算、poll／故障／關閉間隔與文字限制也集中於此，保留原值。`Limits`／`QueueLimits` 仍接受呼叫端覆寫；固定串流索引與 UTF-8 編碼常數留在 OutputHub。

Local Shell settings now include line/argument/history/job limits, output batch sizes,
command/completion deadlines, terminal polling, headless backoff, and shutdown
collection/reporting intervals. Their values are unchanged; shutdown polling does
not impose an overall shutdown timeout. Session identities and terminal display
constants remain in Shell.

本地 Shell 的行／參數／歷史／工作上限、輸出批次、命令／補全期限、終端輪詢、無終端退避與關閉收集／回報間隔皆集中，保留原值。關閉輪詢不引入總關閉期限，Session 身分與顯示常數仍由 Shell 維護。

Socket Shell settings include listener defaults, connection/session bounds, network
queue budgets, output subscription limits, polling and final drain deadlines. The
poller capacity derives from the maximum live clients plus its listener; script
mailbox capacity covers retained remote owners plus the local owner. Tcl output,
delivery/result bounds and cancellable sleep slices are also centralized. Existing
values and `SocketShellOptions` overrides are preserved; recovery-token dimensions
and wire-format constants stay in Shell.

Socket Shell 設定包含 listener、連線／Session 上限、網路佇列、輸出訂閱、poll 與最後排空期限。Poller 容量由最大客戶端加 listener 推導，script mailbox 涵蓋保留遠端 owner 與本地 owner。Tcl 輸出、交付／結果限制及可取消休眠切片也集中；原值與 `SocketShellOptions` 覆寫保留，恢復 token 尺寸與 wire 常數留在 Shell。

Protocol/layout constants remain in the owning component; test workloads remain in
tests. These are compile-time settings, not dynamic updates, persistence, or parameter
registration. Publisher options and BusManager limits can still be supplied through their
existing APIs.

協定／布局常數留在所屬元件，測試負載留在測試。這些是編譯期設定，不會自動提供動態調整、保存或 Param 註冊；Publisher options 與 BusManager limits 仍可由既有 API 傳入。

Ordinary Module Socket failure reporting uses `module_error_report_timeout` (500 ms), without overriding an earlier stop deadline. Contract faults use shared records, not this Socket timeout.

Module 一般 Socket 故障回報等待使用 `module_error_report_timeout`（500ms），不覆蓋停止期限。
契約故障改用共享紀錄，不使用這個 Socket 等待期限。

ModuleSystem migration verification (2026-10-04): Windows x64 Debug/Release full
builds and 33/33 CTest suites passed in each configuration. The added Module,
Control Channel and Message Bus metadata providers share the selected static catalog;
`module.error_protocol` verifies numeric cause-chain transport and local truncation.
These results do not establish Linux/macOS support or reliable delivery after a crash.

Module System 遷移驗證（2026-10-04）：Windows x64 Debug／Release 完整建置及全套各 **33/33** 通過。Module、Control、Bus provider 共用選定靜態 catalog；`module.error_protocol` 驗證原因鏈傳輸與本地截斷。這不代表 Linux／macOS 支援或 crash 後可靠送達。

<a id="contract-violations"></a>

## Contract violations / 契約違反

`<vinc/contract.hpp>` provides `[[noreturn]] vinc::contract_violation(reason, location)`
and `contract_index_violation(reason, index, size, location)`. Both are fatal in
Debug and Release. Neither constructs a framework exception, formats text, allocates
owned strings, nor writes synchronously to stderr before termination.

`<vinc/contract.hpp>` 提供 `[[noreturn]] vinc::contract_violation(reason, location)` 與 `contract_index_violation(reason, index, size, location)`。Debug／Release 都致命，不建立 framework exception、不格式化、不配置字串，也不先同步寫 stderr。

Common remains standard-library-only. Its internal `detail/runtime_services.hpp`
defines one framework bootstrap binding per executable/shared-library image, not a
public handler registry. It replaces the former ContractReporter. ModuleSystem
binds before application construction and clears after runtime services finish.
Normal successful accesses do not read the binding or increment the reader counter.
Only fatal calls use these lock-free local atomics. There is no added Common worker.

Common 只依賴標準庫。內部 `detail/runtime_services.hpp` 每個 executable／shared-library image 只有一個框架啟動綁定，取代 ContractReporter，不是公開 registry。Module System 於應用建構前綁定、服務結束後清除。成功存取不查綁定或增加 reader counter；只有 fatal 使用無鎖本地 atomic，不新增 worker。

With the Module runtime binding, a violation publishes a bounded shared record and
exits the child without invoking user hooks/destructors. With no binding (including
the central process and standalone Common consumers), or when the callback returns,
the fallback is now `std::_Exit(77)`. User terminate handlers, automatic/static
destructors, atexit handlers and stdio flushes are bypassed. This approved change
applies only to framework contract violations, not ordinary C++ exception handling.
No hard OS exit latency is promised. This is not a signal handler or crash catcher.

有 Module 綁定時，違約會發布有界共享紀錄，不呼叫使用者 hook／解構就退出子程序。沒有綁定（含中央與獨立 Common）或 callback 返回時，fallback 為 `std::_Exit(77)`，略過 terminate handler、自動／靜態解構、atexit 與 stdio flush。此政策只限框架契約違反，不改一般 C++ 例外。不保證 OS 硬性退出延遲，也不是 signal handler／crash catcher。

Use these entry points only for broken programming contracts, such as invalid unchecked
indices or expired message access. Recoverable runtime failures continue to use Result.
Reason pointers must refer to readable, terminated text through the call. Source locations
identify the framework detection site, not a captured user stack trace. Standard allocation,
third-party and user-defined exceptions retain their existing boundaries.

只用於已破壞的程式契約，例如非法 unchecked index 或已失效訊息；可恢復失敗仍回 Result。Reason pointer 在呼叫期間須指向可讀、已終止文字。Source location 是框架偵測處，不是使用者堆疊；標準配置、第三方與自訂型別例外維持原邊界。

Module fatal delivery, central retention, and Shell display evidence are recorded in [Module System validation](../module_system/README.md#共享故障紀錄驗證). Earlier catalog tests are not evidence for cross-process fatal reporting.

本輪 Module 致命回報、中央保留與 Shell 顯示的驗證，集中於
[ModuleSystem 驗證](../module_system/README.md#共享故障紀錄驗證)，不把原先 catalog
測試結果視為跨程序致命回報的證據。

<a id="runtimeservices-bridge2026-10-06"></a>

## RuntimeServices bridge（2026-10-06） / RuntimeServices 橋接（2026-10-06）

`detail::RuntimeServices` is a Host-owned immutable table containing version, byte size, an opaque context pointer and one `report_contract` callback. Version 1 supports only contract fatal reporting. Ordinary Message Bus, Param and other services use the implemented Context; there is no registry or automatic service discovery.

`detail::RuntimeServices` 是 Host 持有的不可變表，包含版本、byte 大小、不透明 context pointer 與單一 `report_contract` callback。v1 只做契約 fatal 回報；Message Bus、Param 等一般服務仍走已實作的 Context，不新增 registry 或自動服務探索。

The callback takes a borrowed `ContractViolation`: reason/file/function pointers,
line, has-index flag and bounds. `contract.cpp` flattens source_location before
the call, so no STL ownership or source_location representation crosses the DLL
boundary. Pointers are valid only in the same process and for the duration of the
call. This requires matching architecture/toolchain/build contracts; the version
and size checks do not establish cross-compiler compatibility or a stable Plugin ABI.

Callback 借用 `ContractViolation` 的 reason／file／function pointer、行號、has-index 與界限。`contract.cpp` 呼叫前攤平 source_location，沒有 STL 所有權或其表示法跨 DLL。Pointer 僅限同程序與當次呼叫有效，須符合架構／工具鏈／組態契約；版本／大小檢查不構成跨編譯器或穩定 Plugin ABI。

<a id="binding-and-lifetime"></a>

### Binding and lifetime / 綁定與生命週期

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

- `bind_runtime_services()` 回 `Result<void>`；不支援版本、表太小或缺 callback 時拒絕且不消耗綁定機會。呼叫端提供真實可讀物件，不是解析不可信 bytes；同版本較大表可有未使用尾端。
- 每個 image 一生只成功綁定一次，clear 後也不能重綁；不支援替換／熱卸載。
- 初始化與清除由框架串行，在 DLL loader hooks 外、應用工作前綁定。Host 表、程式碼與 context 須活到所有借用 image 清除且 callback 結束。
- 先停／join Plugin 工作。`clear_runtime_services()` 撤下本地 pointer，等待已進入的 fatal reader，保護借用狀態；不取消 callback 或釋放其資料。新呼叫見 null 直接 fallback；卡住的 callback 可阻止 clear 返回，由中央既有監督負責終止。
- 載入期靜態建構子可能早於注入，使用 fallback，無共享回報保證；clear 後同樣如此。
- EXE／DLL 各自靜態連結 Common bridge，只有綁定／reader count 是本地，兩者指向同一 Host state。`VINC_LOCAL` 在 GCC／Clang 隱藏 bridge／fatal 符號以避免 ELF interposition，Windows 不匯出。Common 採 PIC，正式 Plugin 建置依 Module System 契約。

<a id="error-metadata-and-verification"></a>

### Error metadata and verification / 錯誤 metadata 與驗證

`ErrorDomain::Common = 9` is appended without renumbering prior domains.
`common_error.hpp` supplies typed invalid-table, incompatible-version and
already-bound errors. `vinc_common_errors` registers a header-only static catalog;
Common runtime does not depend on the generated catalog or ModuleSystem.
Fatal dispatch itself never formats an Error or allocates a diagnostic string.

`ErrorDomain::Common = 9` 以追加方式加入，舊 domain 不重編。`common_error.hpp` 提供表無效、版本不相容與已綁定錯誤；`vinc_common_errors` 登記純標頭靜態 catalog。Common runtime 不依賴生成 catalog 或 Module System；fatal 本身不格式化 Error 或配置診斷字串。

`common.runtime_services` checks validation, duplicate binding, no rebind after
clear, unbound/cleared/returning-callback fallback, and bypass of terminate/atexit/
destructor sentinels. Fatal probes also disable ordinary C++ allocation; the bound
probe validates context, bounds and source location without allocating.
Actual two-DLL tests and platform verification are recorded in
[ModuleSystem](../module_system/README.md#runtimeservices-bridge-接線2026-10-06).

`common.runtime_services` 檢查表驗證、重複綁定、clear 後拒絕重綁、無綁定／清除／callback 返回的 fallback，以及略過 terminate／atexit／解構。Fatal probe 關閉一般 C++ 配置，綁定 probe 不配置地核對 context／界限／位置。真正雙 DLL 與平台結果見 [Module System](../module_system/README.md#runtimeservices-bridge-接線2026-10-06)。

<a id="production-plugin-integration"></a>

### Production Plugin integration / 正式 Plugin 接線

The Module System now uses this bridge in its production ModuleHost loader.
Initialization happens after native loading and before Module creation. Plugin
clear follows Module destruction and Host worker shutdown, before the Host table
or report storage expires. This does not expand RuntimeServices beyond fatal reporting.
See [Module System](../module_system/README.md) for the matching-build ABI and loader limits.

正式 ModuleHost loader 已使用此 bridge：native 載入後、Module 建立前初始化；Module 解構與 Host worker 停止後，於 Host 表／回報儲存失效前清除 Plugin 綁定。範圍仍只限 fatal，匹配組態 ABI 與 loader 限制見 [Module System](../module_system/README.md)。

<a id="共享資源名稱接線"></a>

## Shared resource-name integration / 共享資源名稱接線

`detail/resource_names.hpp` defines only the standard-type/Result abstraction `ResourceNames::reserve()` and `retire()`. Bus accepts the central name service without depending back on Module System or filesystems. This is neither a runtime error registry nor a public shared-region access API. Pre-creation recording, retirement, and limits belong to [Module System](../module_system/INSTANCE.md); `instance_*` and `module_lock_*` remain temporary config settings.

`detail/resource_names.hpp` 只定義 `ResourceNames::reserve()`／`retire()` 抽象，使用標準型別與 Result。
它讓 Bus 接受中央提供的名稱服務，而不反向依賴 Module System 或檔案系統。
這不是 runtime error registry，也不是使用者可取得框架共享區的公開入口。
資源建立前記錄、名稱退役與上限由 [Module System](../module_system/INSTANCE.md) 維護；
`config.hpp` 的 `instance_*`、`module_lock_*` 是本輪新增的暫時系統設定。

<a id="param-settings-and-catalog-2026-10-08"></a>

## Param settings and catalog (2026-10-08) / Param 設定與 catalog（2026-10-08）

`ErrorDomain::Param = 10` identifies the component-owned static catalog. Common
does not depend on Param runtime. New `param_*` constants in config bound names,
strings, snapshots, file input and pending work; their defaults and limits are
maintained in the [Param README](../param/README.md). Existing framework settings
remain in config; adding Param does not migrate them or enable dynamic rebinding.

`ErrorDomain::Param = 10` 對應元件自有靜態 catalog，Common 不依賴其 runtime。`param_*` 限制名稱、字串、快照、檔案輸入與待處理工作；預設與限制見 [Param](../param/README.md)。加入 Param 不會自動搬移既有設定或啟用動態重綁。

<a id="log-設定與錯誤分類2026-10-08"></a>

## Log settings and error domain (2026-10-08) / Log 設定與錯誤分類（2026-10-08）

`ErrorDomain::Log = 11`; [Log](../log/README.md) owns error/operation enums and the static catalog. The `log_*` budgets/intervals serve logging; existing settings were not migrated to Param.

新增 ErrorDomain::Log = 11，錯誤／操作 enum 與靜態 catalog 由 [Log](../log/README.md) 維護。
config 的 log_* 預算與間隔僅服務新功能；本輪未把既有設定遷入 Param。

<a id="bag-設定與錯誤2026-10-12"></a>

## Bag settings and errors (2026-10-12) / Bag 設定與錯誤（2026-10-12）

`ErrorDomain::Bag=12` and `NativeErrorSource::Sqlite=11` identify Bag/SQLite diagnostics. Bag registers text; Common does not depend on SQLite. `config::bag_*` bounds queue/message bytes, main database size, batches, discovery/pump intervals, metadata/queries, and shutdown; see [Bag](../bag/README.md).

新增 ErrorDomain::Bag=12、NativeErrorSource::Sqlite=11。文字表由 Bag 元件註冊，
common 不依賴 SQLite。config::bag_* 保存 queue／單筆預算、主資料庫大小上限、批次、探索／pump 間隔、
metadata／查詢限制與 Recorder 收尾期限；定義和限制見 [Bag README](../bag/README.md)。

<a id="unsigned-unix-time-and-utc-formatting-2026-10-13"></a>

## Unsigned Unix time and UTC formatting (2026-10-13) / Unsigned Unix 時間與 UTC 格式（2026-10-13）

`Result<uint64_t> unix_nanoseconds() noexcept` samples `std::chrono::system_clock`.
Its epoch is 1970-01-01 UTC; Unix time excludes leap seconds. A pre-epoch reading
or unsigned-nanosecond overflow returns `Common::TimestampOutOfRange / ReadTime`.
Conversion checks before multiplication; it never wraps a negative/signed overflow into a valid timestamp.
Supported clock representations use integral ticks dividing a nanosecond-based second exactly;
unsupported representations fail compilation rather than silently guessing their units.
The OS clock's resolution and correctness are not guaranteed by the nanosecond unit.
The uint64 output format does not extend system_clock's own native representable range.

`Result<uint64_t> unix_nanoseconds() noexcept` 取樣 `std::chrono::system_clock`，epoch 為 1970-01-01 UTC，不含閏秒。早於 epoch 或 unsigned 奈秒溢位回 `Common::TimestampOutOfRange / ReadTime`，乘法前先驗證，不將負數／溢位包成合法值。只支援可精確換算的整數 tick 表示，其他表示編譯失敗而不猜單位。奈秒單位不保證 OS 精度／正確性，uint64 輸出也不擴大 system_clock 本身範圍。

`std::string format_utc(uint64_t)` formats all values, including zero and UINT64_MAX,
as `YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ` (1970 through 2554). It splits days/seconds
before signed chrono conversion, so values beyond signed nanoseconds remain valid.
No local timezone, timezone database, or OS date formatter is involved. Formatting
may allocate/throw under the existing STL exception policy; it has no recoverable input failure.
`Deadline` remains a monotonic local wait deadline. Do not interchange these clock domains.

`std::string format_utc(uint64_t)` 支援所有值，含 0 與 UINT64_MAX，格式 `YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ`，範圍 1970～2554。先拆日／秒再做有號 chrono 轉換，超出 signed nanoseconds 仍有效。不使用本地時區、時區資料庫或 OS 日期格式化；依既有 STL 政策可能配置／拋例外，沒有可恢復輸入失敗。`Deadline` 仍是本地單調等待期限，兩時鐘域不能混用。

Implementation is in `src/time.cpp`; `detail/time.hpp` is a private checked conversion
helper, also used by tests to exercise invalid clocks without changing the OS clock.
Message publication contracts and version changes belong to [Message Bus](../message_bus/README.md#雙時間發布-metadata2026-10-13).

實作在 `src/time.cpp`，`detail/time.hpp` 為私有 checked 轉換 helper，測試用它驗證非法時鐘而不更改 OS 時間。發布與版本契約見 [Message Bus](../message_bus/README.md#雙時間發布-metadata2026-10-13)。

<a id="rpc-共用接線2026-10-14"></a>

## Shared RPC wiring (2026-10-14) / RPC 共用接線（2026-10-14）

`ErrorDomain::Rpc = 14`; RPC owns its error/operation catalog, with no Common-to-RPC-runtime dependency. `rpc_*` bounds shared/local capacity, pending work, and waits; defaults are in [RPC resources](../rpc/README.md#資源與執行緒). Existing settings have not moved to Param.

新增 ErrorDomain::Rpc = 14；元件自行維護錯誤／操作 catalog，common 不反向依賴 RPC runtime。
config.hpp 的 rpc_* 設定集中控制共享／本地容量、待處理數與等待，預設值及限制見
[RPC 資源與執行緒](../rpc/README.md#資源與執行緒)。未將既有設定遷入 Param。

Module Graph's `ui_graph_*` limits are defined here but owned and documented by the
[UI contract](../ui/README.md#module-graph-2026-10-27--module-關係圖). They do not move
existing configuration to Param. Control wire page sizes remain protocol constants.

Module Graph 的 `ui_graph_*` 暫存設定由 UI 契約維護；本輪未搬移既有 config 到 Param。
控制封包分頁大小仍是各元件的協定常數。

Notification retry/report settings remain compile-time system policy in config.hpp; defaults, meanings and validation belong to [Pump wakeup](../module_system/PUMP_WAKEUP.md).

通知重試／回報間隔暫留 config.hpp；預設值、用途及驗證由上述跨元件契約維護。

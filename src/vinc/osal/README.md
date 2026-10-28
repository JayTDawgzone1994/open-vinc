<a id="osal-process"></a>

# OSAL / 作業系統抽象層

OSAL exposes a common C++23 process API through `<vinc/osal/process.hpp>` and target `vinc::osal`. CMake selects the target-OS backend. Public headers expose neither native process types nor platform conditional compilation.

OSAL 提供同一份 C++23 程序操作介面。框架只使用
`<vinc/osal/process.hpp>` 並連結 `vinc::osal`；CMake 依目標 OS 選擇後端。
公開標頭不包含原生程序型別或平台條件編譯。

Facilities include process management, [Socket](SOCKET.md), [Pipe](PIPE.md), [Shared Memory](SHARED_MEMORY.md), and [FileLock](FILE_LOCK.md). The upper [Concurrency layer](../components/concurrency/README.md) composes SharedMemory/SharedWord into a mutex; OSAL does not depend on it. `<vinc/osal/terminal.hpp>` provides terminal modes, deadline-aware text/key/EOF events, and Windows/POSIX mode restoration; see [Shell](../shell/README.md). `<vinc/osal/random.hpp>` fills caller buffers with `random_bytes(span<byte>)`: BCrypt on Windows, getentropy on Linux/macOS. It defines no framework Session/token format; failure retains native diagnostics and may leave partial bytes, which must not be used as a valid token. [Module System](../module_system/README.md) owns readiness, heartbeat, and restart policy. SharedMemory owns raw byte mapping/lifetime, not [message layout](../components/message_storage/README.md), containers, or [Bus synchronization/reclamation](../message_bus/DESIGN.md).

目前提供程序管理、[通用 OSAL Socket](SOCKET.md)、[通用 OSAL Pipe](PIPE.md)
及 [OSAL Shared Memory](SHARED_MEMORY.md)，另有通用
[OSAL FileLock](FILE_LOCK.md)。跨程序 Mutex 由上層[共用同步庫](../components/concurrency/README.md)
組合 SharedMemory／SharedWord 實作，OSAL 不依賴該庫。`<vinc/osal/terminal.hpp>` 另提供 Shell 所需的
終端模式、具期限的文字／按鍵／EOF 事件，以及 Windows／POSIX 模式還原；使用邊界見
[Shell 說明](../shell/README.md)。
`<vinc/osal/random.hpp>` 提供通用 `random_bytes(span<byte>)`：以 OS 隨機來源填入呼叫端緩衝區，
Windows 使用 BCrypt、Linux／macOS 使用 getentropy；不包含框架 Session 或憑證格式。
失敗保留原生錯誤；緩衝區可能已部分寫入，不可將失敗結果當成有效 token。
上層的 [Module 執行環境](../module_system/README.md) 已接入獨立程序、就緒握手、雙向 heartbeat
與重啟政策；這些行為由 Module／共用通訊層處理，不屬於 OSAL 的程序原語。
Shared Memory 提供原始 bytes 的映射與資源生命週期；訊息布局與容器由上層
[Message Storage](../components/message_storage/README.md) 負責，跨程序讀寫同步與占用回收協定見
[Message Bus 設計](../message_bus/DESIGN.md)。這些功能不屬於 OSAL Shared Memory 的職責。

<a id="標頭與實作位置"></a>

## Headers and implementation / 標頭與實作位置

The generic cross-process [Notification](NOTIFICATION.md) API is available through `<vinc/osal/notification.hpp>`. Windows and POSIX backends are implemented; validation and platform limits are recorded below. Bus/Context integration is implemented outside OSAL; its separate contracts and validation are in [Pump wakeup](../module_system/PUMP_WAKEUP.md).

通用跨程序[通知原語](NOTIFICATION.md)已實作 Windows 與 POSIX 後端，平台驗證與限制見下方紀錄；Bus／Context 接線已於 OSAL 之外實作，契約及驗證另見 [pump 喚醒](../module_system/PUMP_WAKEUP.md)。

Public headers remain `<vinc/osal/...>`. Native/state/platform headers are under `include/vinc/osal/detail/`; shared and platform `.cpp` files stay in `src/`. Detail is for this component and designated tests. Directory cleanup does not change platform behavior; the error contract follows.

公開標頭維持 `<vinc/osal/...>`。native、state 與平台內部標頭統一放在 `include/vinc/osal/detail/`；
`src/` 保留共用及各平台的 .cpp。detail 僅供本元件實作與指定測試使用，
元件目錄整理不改變平台行為契約；本輪錯誤型別遷移見下節。

<a id="統一錯誤介面"></a>

## Unified error interface / 統一錯誤介面

OSAL uses `vinc::Result<T>`/`vinc::Error`, not a separate result or payload. `<vinc/osal/error.hpp>` owns `vinc::osal::ErrorCode` and `ErrorOperation` for `ErrorDomain::Osal`; no-data operations use `Result<void>`. Include `<vinc/result.hpp>` for results and `<vinc/time.hpp>` for `vinc::Deadline`, the local `steady_clock::time_point` cutoff. The former forwarding header was replaced by component enums. Deadline is not calendar time or an IPC field; waiting/cancellation/timeout behavior is unchanged.

OSAL 使用 `vinc::Result<T>`／`vinc::Error`，不提供獨立 Result 或 Error payload。
`<vinc/osal/error.hpp>` 定義 `vinc::osal::ErrorCode` 與 `vinc::osal::ErrorOperation`，
對應 common 的 `ErrorDomain::Osal`；無回傳資料的操作使用
`vinc::Result<void>`。錯誤與結果直接引用 `<vinc/result.hpp>`；截止時間使用
`<vinc/time.hpp>` 的 `vinc::Deadline`，即 `std::chrono::steady_clock::time_point`。
舊的錯誤轉接標頭已由本元件 enum 定義取代。Deadline 是本地等待的截止時間，不是日期時間，
也不直接序列化為跨程序協定欄位；原有等待、取消與逾時行為保持不變。

`Error` stores `domain`, `code`, `operation`, and native source/value. Use domain-aware comparisons such as `error.is(osal::ErrorCode::Timeout)`; OSAL owns its text tables. `vinc::error_to_string(error)` formats diagnostics and does not print misleading native success text when no native value exists. Unknown codes/operations remain numeric. There are 23 error codes and 54 nonzero operations with stable explicit assignments; domain is part of identity. These are incompatible with old implicit enum ordinals, and C++ Error layout is not a cross-version wire format. Existing timeout, cancellation, lifecycle, classification, and exception boundaries remain.

`Error` 的 `domain`、`code`、`operation`、`native` 保存分類、失敗操作與原生來源／數值。
使用 `error.is(osal::ErrorCode::Timeout)` 等具 domain 的比較，文字表由 OSAL 自行維護。
`vinc::error_to_string(error)` 統一產生診斷文字；原生錯誤沒有值時，不輸出「成功」之類
容易誤導的 OS 訊息。未知錯誤／操作保留數值，程式判斷仍使用具型別的錯誤碼。
目前有 23 個錯誤碼與 54 個非零操作碼；既有明確指定的編號保持不變，domain 為完整身分的一部分。
它們不相容於更早期 enum 的隱含序號；C++ Error 布局仍不是跨版本傳輸協定。
現有逾時、取消、生命週期、平台錯誤分類與例外邊界保持原樣。

Common provides fixed-capacity cause chains, default depth 3 including the current layer. Upper components use `wrap_error()` to retain OSAL causes. When OSAL combines different operations, the outer operation describes the caller-facing API and lower failure steps remain causes; same-operation helpers/pass-throughs do not wrap again. Private `detail::operation_error()` accepts only OSAL causes, retaining code/native data without allocation. Other components choose their own code/operation before wrapping. Error remains standard-layout for Windows Pipe I/O containers. Changing capacity requires rebuilding all connected C++ consumers; see [Common](../common/README.md#bounded-cause-chains). The test-only `osal.error_process` format covers counts, omission flags, unknown/long chains; those tests did not change production Module/Bus protocols.

Common 現在提供固定容量的錯誤原因鏈，預設包含目前層共 3 層；
上層以 `wrap_error()` 轉接 OSAL 錯誤時可保留本層診斷。OSAL 內部組合不同操作時，
最外層 operation 表示目前 API 對 caller 承諾的操作，實際失敗步驟保留於 cause chain。
同一操作的 helper 或沒有新增語意的轉交直接傳遞，不重複包裝。
內部 `detail::operation_error()` 僅接受 OSAL 原因，保留原錯誤碼及原生來源／數值，
不配置記憶體；其他元件仍須選擇自己的錯誤碼與操作再使用 `wrap_error()`。
`Error` 保持 Windows Pipe 原生 I/O 容器所需的 standard-layout。
容量變更須重建所有相連的 C++ 元件，詳見 [Common](../common/README.md#bounded-cause-chains)。
`osal.error_process` 的測試專用協定已加入原因筆數、省略旗標、未知原因及超長鏈案例；
正式 Module／Message Bus 的傳輸協定本輪不變。

Integrated operation boundaries:

已接線的組合邊界：

- Socket accept/connect/send/receive `*_until()` retain their public operation; lower Poller initialization, registration, wait, and wake failures remain causes. Direct Poller calls still report Poller operations.
- Socket creation, post-accept native setup, connect peer queries, hostname resolution, and Poller creation preserve the outer API operation and underlying step.
- Preliminary resource failures in `spawn_with_pipe()`, `spawn_with_pipes()`, and `spawn_with_output()` report Spawn; inherited-endpoint adoption reports AdoptPipe. Direct failures do not add redundant same-name causes.
- Windows Pipe random failure is CreatePipe ← RandomBytes; process helpers produce Spawn ← CreatePipe ← RandomBytes, subject to configured capacity/omission.
- Pipe submission setup failures retain ReadPipe/WritePipe outside AdoptPipe, with native diagnostics and caller buffer ownership preserved; see [Pipe contracts and tests](PIPE.md).

- Socket 的 accept／connect／send／receive 等 `*_until()` 保留其公開操作，
  下層 Poller 的初始化、登記、等待或喚醒失敗保留於原因鏈；直接呼叫 Poller 仍回報 Poller 操作。
- Socket 建立、accept 後的原生設定、connect 的 peer 查詢、hostname 解析及 Poller 建立，
  均保留當前 API 操作與下層失敗步驟。
- `spawn_with_pipe()`、`spawn_with_pipes()`、`spawn_with_output()` 的前置資源失敗以 Spawn 回報；
  繼承端點接手失敗以 AdoptPipe 回報。直接的 Spawn／AdoptPipe 失敗不再增加同名原因。
- Windows Pipe 亂數失敗為 CreatePipe ← RandomBytes；經程序建立便利 API 回報時，
  為 Spawn ← CreatePipe ← RandomBytes（依設定容量保留或標記省略）。
- Pipe 首次提交設定失敗保留 ReadPipe／WritePipe ← AdoptPipe 與原生診斷，且不移走
  呼叫端 buffer；斷管與訊號隔離的共同契約及驗證記錄見 [Pipe](PIPE.md)。

These changes preserve Timeout/Cancelled codes, check order, cancellation results, cleanup, and exception boundaries. Callers branch on the current layer; causes provide diagnostics rather than a dependency on internal implementation steps.

這些變更不改變 Timeout／Cancelled 等錯誤碼、等待順序、取消結果、回收及例外邊界。
呼叫端以目前層作程式判斷；原因鏈供診斷，不需依賴內部失敗步驟來控制流程。

Recorded 2026-10-04: Windows x64/MSVC 19.51.36257, SDK 10.0.26100.0, CMake 4.3.2, Ninja 1.13.2. Full Debug/Release builds and public headers passed, with **30/30** CTests each. Clean standalone OSAL Release including Common passed **11/11**; standalone Common without an OSAL provider passed **2/2**. Negative catalog/CMake cases and explicit shared error fields decoded after child exit were tested. This was a test-only codec, not production Module/Bus changes. NTSTATUS tests used synthetic values, not real BCrypt failure injection. Current platform results are listed in [Current platform validation](#current-platform-validation).

本輪驗證（2026-10-04）：Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、
CMake 4.3.2、Ninja 1.13.2。完整 Debug／Release 建置及公開標頭編譯成功，全套 CTest
各 **30/30** 通過；獨立 OSAL Release 從乾淨目錄建置，common 與 OSAL 測試 **11/11** 通過。
獨立 common Release 無 OSAL provider 的建置與 **2/2** 測試亦通過。新增驗證包括靜態表格
與 CMake 接線的負向案例，以及子程序退出後，由父程序解讀共享記憶體中的錯誤欄位。
該編碼是測試專用格式，沒有修改 Module／Message Bus 的正式協定。
原生 NTSTATUS 保留來源與數值，測試使用合成值，未注入 BCryptGenRandom 的實際失敗。
目前平台結果見[目前平台驗證](#current-platform-validation)。

<a id="utf-8-與原生路徑"></a>

## UTF-8 and native paths / UTF-8 與原生路徑

`<vinc/osal/path.hpp>` provides `path_from_utf8(std::string_view)` and `path_to_utf8(const std::filesystem::path&)`, returning `Result<std::filesystem::path>` and `Result<std::string>`. Link `vinc::osal`; the SDK umbrella includes this header.

`<vinc/osal/path.hpp>` 提供 `path_from_utf8(std::string_view)` 與
`path_to_utf8(const std::filesystem::path&)`，分別回傳 `Result<std::filesystem::path>`
及 `Result<std::string>`，連結既有 `vinc::osal`；SDK 整合入口亦包含此標頭。

- Accept strict UTF-8 only; never guess Big5/ANSI/locale or replace invalid characters. Invalid encoding returns `InvalidEncoding=23`; embedded NUL returns `InvalidArgument` to prevent native truncation.
- Unrepresentable native paths also return InvalidEncoding. This limits conversion APIs only; native-path OSAL APIs remain available.
- Empty inputs convert successfully. Relative paths, `..`, native separators, and prefixes are preserved. No absolutization, normalization, Unicode normalization, or filesystem access occurs; success proves neither filename validity, existence, permission, nor cross-OS usability.
- Windows uses `CP_UTF8` with strict validation to/from UTF-16. Length beyond native `int` returns `MessageTooLarge`; other failures retain Win32 data. Linux/macOS validate and preserve native char bytes without locale conversion.
- Returned storage is owned and may allocate; existing STL exception policy applies. The APIs are not `noexcept` and add no framework exceptions.
- Stable operations are `PathFromUtf8=44`, `PathToUtf8=45`. Path, Process, and Local Socket share a private UTF-8 validator but retain their own NUL/capacity rules.

- 嚴格接受 UTF-8，不猜 Big5／ANSI／locale，不以替代字元修補錯誤。非法編碼回
  `InvalidEncoding=23`；內嵌 NUL 回 `InvalidArgument`，防止 OS 操作時路徑被截斷。
- 原生路徑若不能表示成合法 UTF-8，同樣回 `InvalidEncoding`。此限制只屬於轉換 API；
  既有直接接受 `std::filesystem::path` 的 OSAL API 保留。
- 空文字／空 path 可成功轉換。保留相對路徑、`..`、原生分隔符及路徑前綴；不做
  絕對化、正規化或 Unicode normalization，不碰檔案系統。成功不表示檔名合法、
  檔案存在、具備權限或可跨 OS 使用。
- Windows 明確使用 `CP_UTF8` 與嚴格字元檢查旗標，轉換至／自 UTF-16；超過原生
  API 的 `int` 長度範圍回 `MessageTooLarge`，其他原生失敗保存 Win32 來源與數值。
  Linux／macOS 驗證後直接保存原生 char bytes，不做 locale 轉換。
- 回傳值擁有自己的儲存區，可能配置記憶體；STL／配置例外沿用既有政策，不標記
  `noexcept`，也不新增 framework exception。
- 操作碼固定為 `PathFromUtf8=44`、`PathToUtf8=45`，原有編號不變。
  路徑、Process 與 Local Socket 共用內部 UTF-8 驗證器，各自維持 NUL／容量限制。

On Windows, `std::filesystem::path(text)` from narrow text is not an explicit UTF-8 conversion. Use these helpers at UTF-8 boundaries; do not cast char storage to char8_t and read it. Tcl slash adaptation belongs to Shell, not general OSAL.

Windows 的一般窄字元 `std::filesystem::path(text)` 不代表 UTF-8 輸入；
UTF-8 文字入口應明確使用以上 helper。不能將 char 儲存區轉型為 char8_t 指標後讀取。
Tcl 所需的 `/` 分隔符由 Shell adapter 負責，不放進通用 OSAL 契約。

```cpp
auto path = vinc::osal::path_from_utf8(input);
if (!path) {
    return std::unexpected(path.error());
}

// The receiving operation decides whether relative or empty paths are allowed.
return open_resource(*path);
```

<a id="路徑轉換驗證2026-10-05"></a>

### Path conversion validation (2026-10-05) / 路徑轉換驗證（2026-10-05）

`osal.path` covers Chinese/emoji round trips, actual files, empty/relative/prefix preservation, invalid UTF-8/Big5/NUL/native encodings, isolated UTF-16 surrogates on Windows, and invalid native bytes/legal backslash filenames on POSIX. Independent SDK header compilation includes path.hpp. Shell checks local/remote Unicode scripts, deployment directories, and OSAL causes. Windows x64/MSVC 19.51.36257, SDK 10.0.26100.0, CMake 4.3.2, Ninja 1.13.2: full Debug/Release builds/headers passed; related CTests **20/20** each (34.80 s / 34.44 s). Current platform results are listed in [Current platform validation](#current-platform-validation). Native-allocation fault injection and >INT_MAX string allocation were not performed.

新增 `osal.path`：中文／emoji 往返、實際開檔、空值、相對路徑與前綴保留、非法 UTF-8、
Big5 bytes、NUL、無法輸出的原生編碼。Windows 案例含孤立 UTF-16 surrogate；
POSIX 案例含非法原生 bytes 與合法反斜線檔名。SDK 每個標頭獨立編譯亦涵蓋 path.hpp。
Shell 整合另驗證本機／遠端 Unicode 腳本、Unicode 部署目錄與 OSAL 原因鏈。
Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 完整建置與公開標頭編譯成功；相關 CTest 各 20/20 通過
（Debug 34.80 秒、Release 34.44 秒）。目前平台結果見[目前平台驗證](#current-platform-validation)。
未注入原生轉換 API 的配置故障，亦未配置超過 INT_MAX 的測試字串。

<a id="統一操作契約"></a>

## Process operation contracts / 統一操作契約

| Operation | Behavior |
| --- | --- |
| `ProcessService::create()` | Create an owner-held management service and one monitoring thread |
| `spawn(options)` | Launch the specified executable and obtain a Process handle |
| `find(id)` | Reacquire control through the service-issued stable ProcessId |
| `try_wait()` | Inspect exit; return an empty optional while running |
| `wait_until(deadline, token)` | Wait for exit, timeout, or cancellation without terminating the child |
| `terminate()` / `terminate(id)` | Request forced OS termination without child cooperation |
| `forget(id)` | Remove completed-observation records; Busy while still running |
| `shutdown_until(deadline, token)` | Stop new admission and wait for accepted launches/observations; no implicit termination |

| 操作 | 行為 |
| --- | --- |
| `ProcessService::create()` | 建立中央持有的管理服務與一條監看執行緒 |
| `spawn(options)` | 啟動指定執行檔，取得 `Process` 控制物件 |
| `find(id)` | 依服務發出的穩定 `ProcessId` 重新取得控制物件 |
| `try_wait()` | 查詢退出狀態；仍執行時回傳空的 `optional` |
| `wait_until(deadline, token)` | 等待退出、逾時或取消；不會因此終止子程序 |
| `terminate()` / `terminate(id)` | 立即要求 OS 強制終止，不要求子程序配合 |
| `forget(id)` | 移除已完成觀察的紀錄；仍執行時回報 `Busy` |
| `shutdown_until(deadline, token)` | 關閉新建程序入口，等待已接受的建立操作及子程序完成觀察；不隱含終止 |

`Requested` from terminate means the OS accepted the request, **not confirmed exit**; call wait_until to confirm. Already-observed exit returns AlreadyExited. Timeout/cancellation preserve management so the owner can terminate, wait, and inspect later. Deadlines use steady_clock. Native launch itself has no cancellable or hard real-time deadline guarantee.

`terminate()` 回傳 `Requested` 表示 OS 已接受要求，**不表示退出已完成**；
接著使用 `wait_until()` 確認。已觀察到退出時回傳 `AlreadyExited`。
逾時或取消不會丟失管理權；中央仍可終止、等待與查詢。所有期限使用
`steady_clock`。原生啟動呼叫本身沒有可取消或硬即時期限的保證。

Successful spawn means a managed child exists, not that Module initialization completed. Native launch errors are returned; if POSIX represents a later launch failure by child exit, retain that status rather than interpreting every 127 as one specific error.

`spawn()` 成功表示子程序已建立並納入管理，不表示模組初始化完成。
原生啟動失敗以錯誤回報；POSIX 若以新程序退出碼呈現後續啟動失敗，則保留
該退出碼，不把 127 一律解讀成某一種錯誤。

<a id="所有權與回收"></a>

## Ownership and cleanup / 所有權與回收

**Observation failure and control capability (fixed 2026-10-05).**

**觀察失敗與控制能力（2026-10-05 修正）。**

- Ordinary native-poll failure retains ownership: the same Windows process handle or POSIX child identity while exclusive wait/reap ownership remains. Terminate can still request OS action.
- The monitor retries, with an extra 50 ms backoff per pass after an observation failure while ownership remains, preventing a permanently ready watch from spinning. No service/record lock is held during backoff; queries/termination remain usable.
- try_wait/wait_until return the first observation error until exit is confirmed; later waits then return the actual ExitStatus. `observation_error()` retains the first diagnostic after recovery. Successful terminate neither clears it nor proves exit; forget remains Busy while ownership remains.
- POSIX ECHILD means exclusive observation ownership was lost: retain ObservationLost and release native ownership, never control by that PID again. Terminate returns ObservationLost/Terminate with cause; waiting never fabricates ExitStatus. The failed observation record can be released, without claiming process exit.
- shutdown waits for all owned observations, can time out while exit is unconfirmed, and still reports the first error in retained records after completion. Inspect diagnostics, forget, then retry. Module System reclaims shared occupancy only from genuine ExitStatus.
- Invalid handles, permanent failure, or external waitpid/SIGCHLD interference may be unrecoverable. Do not guess PIDs, invent exit, or forcibly clear Bus Readers as a substitute.

- 一般 native poll 失敗保留原生 owner；Windows 繼續使用同一 process handle，
  POSIX 繼續使用尚未失去獨占 wait/reap 所有權的子程序身分。`terminate()` 仍可要求 OS 終止。
- 背景監看會重試；曾發生觀察失敗且 owner 尚在時，每輪另退避 50ms，避免持續 ready
  的原生 watch 造成忙迴圈。退避期間不持有 service／record lock，公開終止與查詢仍可執行。
- `try_wait()`／`wait_until()` 在退出未確認前回傳保存的首次觀察錯誤；稍後確認退出後，
  重複等待回傳真正的 ExitStatus。新增 `observation_error()` 可查首次錯誤，恢復後仍保留。
  `terminate()` 成功不清除此錯誤，也不當成退出證明。owner 尚在時 `forget()` 仍回 Busy。
- POSIX `ECHILD` 代表獨占子程序所有權已失去，保存 ObservationLost 並釋放 native owner；
  不再以 PID 嘗試控制。後續 `terminate()` 回 ObservationLost／Terminate 並保留原因，
  等待不會虛構 ExitStatus。此分支仍允許釋放已完成失敗觀察的紀錄；不表示程序已退出。
- `shutdown_until()` 等待所有 owner 完成觀察。未確認退出且 owner 仍在時可逾時；
  觀察完成後仍會回報保留紀錄中的首次錯誤。caller 可先讀診斷再 `forget()`，重試 shutdown。
  上層 Module System 只憑真正 ExitStatus 回收共享占用，見其重審修正紀錄。
- 原生 handle 失效、永久觀察失敗、外部 waitpid／SIGCHLD 政策破壞所有權，均不保證可恢復；
  不使用猜測 PID、假造退出或強制清除 Message Bus Reader 的替代途徑。

`osal.process_faults` uses a test native backend for observation failure, termination, recovery, retained diagnostics, and lost ownership. Existing process/process_output/Module suites cover real children. Related Windows Debug/Release tests passed **9/9**; scope/limits are in [Module System results](../module_system/README.md#本輪驗證結果).

新增 `osal.process_faults` 以測試 native backend 驗證觀察失敗、終止、恢復、
診斷保留與失去所有權；真實子程序另由既有 process／process_output 與 Module 測試驗證。
Windows Debug／Release 相關測試各 9/9 通過，完整項目與限制見
[Module System 本輪驗證](../module_system/README.md#本輪驗證結果)。

- Process is move-only; releasing it neither kills the child nor abandons observation. Retain ProcessId to reacquire control through the service.
- Native resources are reclaimed at exit; records retain results for multiple waiters. Existing controls can read saved results after forget.
- max_records defaults to 256, including starting/running/retained records. Exhaustion returns ResourceExhausted; explicitly forget handled records.
- ProcessId is not reused during one loaded OSAL lifetime. pid() is diagnostic only; bare PIDs cannot take over/terminate external processes.
- Releasing ProcessService closes admission. Monitor state survives until all owned observations finish; non-exiting children keep it alive. Explicitly terminate/wait during shutdown, then call shutdown_until.
- Only directly launched children are managed, not descendant trees. This is not a permission sandbox and cannot override OS privileges/kernel state to guarantee termination.

- `Process` 可移動、不可複製。釋放它不會殺死子程序，也不會放棄監看。
  中央保存 `ProcessId` 後仍可透過服務重新取得控制物件。
- 退出時回收原生程序資源；結果保存於管理紀錄，多個等待者可取得相同結果。
  `forget()` 後既有控制物件仍可讀取保存的結果。
- `max_records` 預設 256，包含啟動中、執行中及保留結果的紀錄；額滿回報
  `ResourceExhausted`。中央處理完結果後應明確 `forget()`，釋放容量。
- `ProcessId` 在同一份已載入 OSAL 的執行期間不重用；`pid()` 僅供診斷，
  不接受以裸 PID 接管或終止外部程序。
- 釋放 `ProcessService` 會停止接受新程序。背景監看狀態存活到現有子程序
  全部完成觀察；子程序若不退出，監看狀態也會繼續存在。中央應在正常
  關閉流程中明確終止／等待程序，再呼叫 `shutdown_until()`。
- 管理對象是直接啟動的單一子程序，不包含它自行建立的整棵程序樹。
  此功能也不是權限沙箱，不能保證越過 OS 權限或核心狀態強制結束程序。

Ordinary operations may run concurrently on live services/controls; native wait/reap/termination of one record is serialized. Synchronize moves, destruction, and mutation of SpawnOptions against other users. A spawn admitted before closure may finish; later admission returns ServiceClosed.

同一個存活的服務與控制物件允許並行呼叫一般操作；同一紀錄的原生等待、
回收與終止會序列化。移動、解構物件，以及修改傳入的 `SpawnOptions`，
必須由呼叫端與其他使用者同步。關閉前已接受的 `spawn()` 可以完成；
關閉後才嘗試進入的建立操作回報 `ServiceClosed`。

Integration is currently static. No contract permits unloading OSAL while running; shutdown_until finishes observation, not a dynamic-library safe-unload barrier.

目前以靜態庫整合，不提供執行中卸載 OSAL 動態庫的契約。
`shutdown_until()` 確認程序觀察已完成，並非動態庫安全卸載屏障。

<a id="啟動參數與錯誤"></a>

## Launch options and errors / 啟動參數與錯誤

`SpawnOptions::new_process_group` defaults false; ModuleManager sets true. Windows creates a new group and disables child CTRL_C_EVENT; POSIX creates a child-PID group separate from the parent foreground group. This allows central shutdown coordination without a new console/session; terminate still targets only the managed child. CTRL_BREAK_EVENT and individually directed signals are outside this isolation. See [Windows flags](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags) and [POSIX spawn](https://man7.org/linux/man-pages/man3/posix_spawn.3.html).

- `SpawnOptions::new_process_group` 預設 `false`；ModuleManager 啟動模組時設為 `true`。
  Windows 建立新群組並停用子程序的 CTRL_C_EVENT；POSIX 使用子程序 PID 建立新程序群組，
  與父程序的終端前景群組分離。目的為讓中央協調關閉；不建立新 console／session，
  `terminate()` 仍只處理直接管理的程序。Windows CTRL_BREAK_EVENT 及對子程序個別送出訊號不在此隔離保證內。
  平台語意見 [Windows 建立旗標](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags)
  與 [POSIX spawn](https://man7.org/linux/man-pages/man3/posix_spawn.3.html)。

- Executable/optional working directory are absolute filesystem paths. No PATH search or shell is involved. UTF-8 args exclude argv[0] and reject NUL/invalid UTF-8. Windows converts to UTF-16 with Microsoft CRT/wmain quoting; third-party raw-command-line parsers may differ.
- Working directory affects only the child. Missing POSIX spawn-chdir extensions return Unsupported; never change parent cwd as a workaround.
- Environment inherits by default or can be completely replaced. Apply entries in order; nullopt deletes. UTF-8 names/values follow OS case rules (Windows insensitive, POSIX sensitive); portable callers avoid names differing only by case.
- stdin defaults Null, stdout/stderr Inherit; each supports Inherit/Null. Missing inherited streams become Null. Extra handles/fds are not automatically passed. spawn_with_pipes explicitly hands off one-way endpoints; spawn_with_pipe combines two directions. stdout/stderr Capture uses [spawn_with_output](OUTPUT.md), returning separate PipeReaders; ordinary spawn and stdin reject Capture.
- Common Error retains class/operation/native source/value; checks without native errors leave native data empty. OSAL prints no logs.
- Allocation and other C++ exceptions may propagate; APIs are not uniformly noexcept. Child ownership objects are preallocated so post-creation allocation failure cannot lose control. Native event-registration failure retains the child with interval-based nonblocking observation.
- ExitStatus preserves POSIX signals/Windows exit codes. Nonzero Windows codes are not automatically classified as crashes; upper layers decide.

- 執行檔及可選工作目錄使用絕對 `std::filesystem::path`，不搜尋 `PATH`，
  不經 shell。參數不含 `argv[0]`，使用 UTF-8，拒絕 NUL 與無效 UTF-8。
  Windows 轉為 UTF-16，依 Microsoft CRT／`wmain` 規則組合參數；自行解析
  原始命令列的第三方程式不一定使用此規則。
- 工作目錄只影響子程序。POSIX 工具鏈若缺少 spawn chdir 擴充，指定工作
  目錄回報 `Unsupported`；不以修改父程序 cwd 作為替代方案。
- 環境預設繼承，也能完整替換；依序套用 entries，`nullopt` 刪除變數。
  名稱與值使用 UTF-8。名稱大小寫遵守 OS 規則：Windows 不區分，POSIX
  區分；可攜程式不應建立僅大小寫不同的兩個名稱。
- 標準輸入預設接空裝置；標準輸出／錯誤預設繼承。每條皆可選 `Inherit`
  或 `Null`；未配置的繼承標準串流改接空裝置。額外的可繼承 handle／fd
  不會自動傳入。`spawn_with_pipes()` 可明確交付指定的單向 Pipe 端點；
  `spawn_with_pipe()` 是雙向便利組合。stdout／stderr 另可選 `Capture`，透過
  [`spawn_with_output()`](OUTPUT.md) 取得獨立 `PipeReader`；一般 `spawn()` 不接受
  `Capture`，stdin 也不接受此模式。
- `vinc::Error` 包含統一分類、失敗操作與原生來源與數值；沒有原生錯誤
  的契約檢查使用空 error code。OSAL 不自行印日誌。
- 記憶體配置等 C++ 例外仍可向呼叫端傳播，API 並非全面 `noexcept`。
  子程序的 C++ 擁有物件皆預先配置，避免建立成功後因配置失敗失去管理權。
  原生事件註冊失敗時保留子程序，改用有間隔的非阻塞觀察。
- POSIX signal 與 Windows exit code 保留於 `ExitStatus`。Windows 非零
  退出碼不被任意分類為「崩潰」，由上層依模組契約判定。

<a id="平台後端與限制"></a>

## Platform backends and constraints / 平台後端與限制

These are internal backend choices; framework callers use the common API above.

以下是 OSAL 內部實作，框架呼叫端使用上面的同一套介面。

| Platform | Creation | Exit notification/reaping | Forced termination |
| --- | --- | --- | --- |
| Windows | CreateProcessW with explicit handle list | thread-pool notification; process-handle wait and exit code | TerminateProcess |
| Linux | posix_spawn, close nonstandard fds | pidfd + epoll; waitpid | pidfd_send_signal(SIGKILL), or owned-child PID without pidfd |
| macOS | posix_spawn with POSIX_SPAWN_CLOEXEC_DEFAULT | kqueue NOTE_EXIT; waitpid | SIGKILL to owned child |

| 平台 | 建立 | 退出通知與回收 | 強制終止 |
| --- | --- | --- | --- |
| Windows | `CreateProcessW`，明確 handle 清單 | thread-pool wait 通知監看執行緒；等待 process handle 並讀取退出碼 | `TerminateProcess` |
| Linux | `posix_spawn`，關閉非標準 fd | pidfd + epoll 通知；`waitpid` 回收 | `pidfd_send_signal(SIGKILL)`；未取得 pidfd 時使用受管理的子程序 PID |
| macOS | `posix_spawn`，`POSIX_SPAWN_CLOEXEC_DEFAULT` | kqueue `NOTE_EXIT` 通知；`waitpid` 回收 | 對受管理的子程序送出 `SIGKILL` |

Linux/macOS registration failure falls back to waitpid(WNOHANG) every 50 ms, retaining reap responsibility; complete monitor failure also degrades, with monitor_error retaining diagnostics. Normal event wakeups scan bounded records. Large-process-count performance is not yet measured.

Linux／macOS 事件註冊失敗時，以 50ms 間隔的 `waitpid(WNOHANG)` 保留回收責任。
整個事件監看器失敗時也會降級；`monitor_error()` 保存這種基礎設施錯誤。
一般事件通知每次喚醒會掃描有界的管理紀錄，尚未提供大量程序的效能量測。

POSIX embedding preconditions:

POSIX 嵌入條件：

- Do not set SIGCHLD to SIG_IGN or enable SA_NOCLDWAIT; creation checks these. No external handler/thread may wait/reap OSAL children; waitpid(-1, ...) can steal them.
- OSAL does not replace the parent signal handler. Children clear the signal mask and reset SIGPIPE to default; other ignored signals inherit POSIX behavior.
- External reaping causing ECHILD reports ObservationLost, stops PID control, and never fabricates success. Handle the cause and explicitly release the record.
- While inheriting environment, the host must not concurrently mutate global environ. Concurrent distinct child environments use per-spawn environment/Replace.

- 不可將 `SIGCHLD` 設為 `SIG_IGN` 或啟用 `SA_NOCLDWAIT`；建立時會檢查。
  不可由其他 handler／執行緒對 OSAL 擁有的子程序呼叫 `wait*()`。
  其他元件使用 `waitpid(-1, ...)` 可能搶先回收 OSAL 子程序。
- OSAL 不改寫父程序全域 signal handler；子程序清除 signal mask，並恢復
  `SIGPIPE` 預設動作。其他原生忽略訊號的繼承依 POSIX 行為保留。
- 外部回收導致 `ECHILD` 時回報 `ObservationLost`，停止使用該 PID 控制程序，
  不假造成功退出。呼叫端需處理原因，再明確釋放紀錄。
- 繼承環境時宿主不得同時修改全域 `environ`；並行啟動不同環境應使用各自
  的 `SpawnOptions.environment` 或 `Replace`。

Linux currently needs glibc posix_spawn_file_actions_addclosefrom_np (2.34+), within Ubuntu 22.04; CMake checks availability. macOS checks its spawn extensions. Toolchains must support the used C++23 expected and stop-token waits. Minimum Windows/macOS deployment versions remain unestablished.

Linux 目前要求 glibc 的 `posix_spawn_file_actions_addclosefrom_np`（2.34 起），
符合 Ubuntu 22.04 的 libc 範圍；CMake 會檢查。macOS 會檢查所需 spawn 擴充。
仍須使用支援 `std::expected`、stop-token 條件等待等功能的 C++23 工具鏈。
Windows／macOS 最低部署版本尚未確立。

Native references: [Microsoft CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw), [POSIX spawn](https://man7.org/linux/man-pages/man3/posix_spawn.3.html), [Apple posix_spawn](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/posix_spawn.2.html), [glibc closefrom](https://sourceware.org/pipermail/glibc-cvs/2021q3/073654.html).

原生契約參考：[Microsoft CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)、
[POSIX spawn / Linux man-pages](https://man7.org/linux/man-pages/man3/posix_spawn.3.html)、
[Apple posix_spawn](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/posix_spawn.2.html)、
[glibc closefrom 擴充](https://sourceware.org/pipermail/glibc-cvs/2021q3/073654.html)。

<a id="使用範例"></a>

## Usage example / 使用範例

```cpp
#include <vinc/osal/process.hpp>
using namespace vinc::osal;
using namespace std::chrono_literals;

// Central keeps this service alive and records each successful child's id().
vinc::Result<Process> start_child(ProcessService& central,
                            const std::filesystem::path& absolute_executable) {
    SpawnOptions options;
    options.executable = absolute_executable;
    return central.spawn(options);
}

vinc::Result<ExitStatus> stop_child(ProcessService& central, ProcessId id) {
    constexpr auto shutdown_timeout = 5s;

    auto child = central.find(id);
    if (!child) {
        return std::unexpected(child.error());
    }

    auto stopped = central.terminate(id);
    if (!stopped) {
        return std::unexpected(stopped.error());
    }

    return child->wait_until(std::chrono::steady_clock::now() + shutdown_timeout);
}
```

The owner creates and retains ProcessService/IDs, then forgets handled results. The same ID remains usable after termination failure or timeout. Once children are handled, shutdown_until completes orderly service shutdown.

中央透過 `ProcessService::create()` 建立並長期持有服務及 ID，處理退出
結果後呼叫 `forget(id)`。終止失敗或等待逾時後，仍可使用相同 ID 繼續處理。
全部程序處理完畢後，以 `shutdown_until()` 完成正常關閉流程。

<a id="建置與驗證"></a>

## Build and recorded validation / 建置與驗證

OSAL builds standalone with adjacent `src/vinc/common`, without the message generator or other VINC runtime components. `vinc::osal` publicly propagates Common so consumers link only the OSAL target. The standalone entry adds the sibling Common if no target exists:

OSAL 可單獨建置，只需一併提供相鄰的 `src/vinc/common`；不依賴 message generator
或其他 VINC 執行元件。`vinc::osal` 以 PUBLIC 相依傳遞 `vinc::common`，消費端只需連結
OSAL target。若上層尚未提供 `vinc::common`，獨立入口會自動加入旁邊的 common：

```sh
cmake -S src/vinc/osal -B build/osal -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/osal
ctest --test-dir build/osal --output-on-failure
```

On Windows use an x64 MSVC developer shell, optionally adding `-DCMAKE_CXX_COMPILER=cl`. This uses command-line CMake without Visual Studio GUI. The full project also includes these targets/CTest; `-DBUILD_TESTING=OFF` disables tests.

Windows 在 x64 MSVC developer shell 執行，可加 `-DCMAKE_CXX_COMPILER=cl`；
全程為命令列 CMake，不需要 Visual Studio GUI。完整專案亦包含此 target
及 CTest；`-DBUILD_TESTING=OFF` 可停用測試程式。

Tests cover immediate exit, retained results, forced termination, multiple waiters, timeouts/cancellation, management after control release, Unicode/quoted args, cwd, environment replacement, stdio, excluded extra handles/fds, abnormal exit, concurrency, repeated cleanup, capacity, and shutdown. Windows also tests exit code 259; POSIX checks reaping.

測試包含立即退出、保存結果、強制終止、多等待者、逾時／取消、控制物件
釋放後繼續管理、Unicode 與引號參數、工作目錄、環境替換、標準串流、
額外 handle／fd 不被繼承、異常退出、並行建立、重複回收、容量及關閉流程。
Windows 另測試退出碼 259；POSIX 檢查子程序已被回收。

<a id="current-platform-validation"></a>

### Current platform validation / 目前平台驗證

| Platform / 平台 | Recorded scope and result / 已記錄範圍與結果 |
| --- | --- |
| [Windows](../../../README.md#windows-validation) | Full Debug/Release builds; selected OSAL/OutputHub/Log CTests **23/23 each** / 全專案建置、選定套件兩種組態各 **23/23** |
| [Linux](../../../README.md#linux-validation) | Ubuntu 24.04 / WSL 2 standalone OSAL Debug; descriptor/Process/SharedMemory/FileLock **4/4**, descriptor repeat **20/20**, five negative controls / 獨立 OSAL Debug、四套通過、重跑與負向對照通過 |
| [macOS](../../../README.md#macos-validation) | Full Debug build; initial full-project validation and follow-up OSAL/OutputHub **24/24** / 完整 Debug 建置、初次全專案驗證與後續 **24/24** |

The linked reports own environment versions, source revisions, logs, and coverage limits. The dated records below preserve individual changes' validation history; they do not replace this current summary. Real native failure injection and the deferred creation/spawn race remain separate concerns.

環境版本、原始碼版本、log 與覆蓋限制以連結的報告為準。以下日期紀錄保留各次修改的歷史驗證，不代表目前平台總狀態；真實原生故障注入與暫緩的建立／spawn 競爭問題仍分別追蹤。

Historical environment, 2026-09-18: Windows x64, MSVC 19.44, SDK 10.0.26100, CMake 3.31.6, Ninja 1.12.1. Current platform results are summarized above.

2026-09-18 的歷史驗證環境為 Windows x64、MSVC 19.44、SDK 10.0.26100、CMake 3.31.6、Ninja 1.12.1；目前各平台結果見上方摘要。

**Native macOS validation, 2026-10-27:** macOS 26.3.1(a), arm64, Debug. The full project rebuilt successfully after the Pipe changes; all 24 OSAL/OutputHub CTests passed, including 9 new contract/fault tests. See the [validated environment and source revision](../../../README.md#macos-validation) and [Pipe test details](PIPE.md#斷管與提交失敗契約測試2026-10-27). This does not establish minimum macOS compatibility, Intel Mac support, Release results, or current Windows/Linux results.

**2026-10-27 macOS 原生驗證：** macOS 26.3.1(a)、arm64、Debug。Pipe 修改後完整專案建置成功，OSAL／OutputHub 的 24 項 CTest 全數通過，包含新增 9 項契約／故障測試。完整工具鏈與原始碼版本見[已驗證環境](../../../README.md#macos-validation)，測試明細見 [Pipe](PIPE.md#斷管與提交失敗契約測試2026-10-27)。本次結果不代表最低 macOS 相容版本、Intel Mac、Release 或本次 Windows／Linux 驗證通過。

<a id="2026-10-04-可讀性收尾第一輪"></a>

### Readability closeout, stage 1 (2026-10-04) / 2026-10-04 可讀性收尾：第一輪

- Windows NativeProcess::spawn gained named command-line, inheritance, monitoring, and creation helpers, preserving handle acquisition/RAII cleanup order and the no-allocation-after-child-creation boundary.
- NativeSocketPoller::create separates wake sockets, connection initiation/completion, readiness, matching-reader acceptance, and setup. Named constants retain the 5 s setup limit/10 ms cancellation polling, deadline origin, and failure order.
- English comments document atomic size/alignment/writable mappings, OVERLAPPED container layout/lifetime, and native handle/token/Socket/Random/kqueue boundaries. Other implementation/header changes were comments only.
- Windows x64/MSVC 19.51.36257, SDK 10.0.26100.0, CMake 4.3.2, Ninja 1.13.2: full Debug/Release builds and **12/12** related tests each (seven osal.*, OutputHub integration, Control, Bus ring, Module process, Shell socket). No Linux/macOS build/run in that round.

- Windows `NativeProcess::spawn()` 拆出命令列準備、繼承清單、退出監控登記與建立程序的
  helper；保留標準 handle 的取得順序、RAII 回收順序，以及成功建立子程序後不再配置記憶體的邊界。
- Windows `NativeSocketPoller::create()` 拆出喚醒 socket 建立、發起／完成連線、
  等待就緒、接受匹配 reader 及設定連線的階段。5 秒建立上限與 10 毫秒取消輪詢間隔改用
  OSAL 內部具名常數；數值、deadline 起算位置與失敗處理順序維持原有行為。
- 補齊原生轉型的英文註解，說明 atomic 的大小／對齊與可寫映射要求、OVERLAPPED
  回推物件的布局及生命週期，以及 native handle、token、Socket、Random 和 kqueue 的邊界。
  除上述兩個 Windows 流程外，其餘實作及標頭只修改註解。
- Windows x64、MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、
  Ninja 1.13.2：完整專案 Debug／Release 建置成功，兩種組態各 **12/12** 相關測試通過。
  範圍為七個 `osal.*` 套件，以及 `output_hub.integration`、`control.channel`、
  `message_bus.ring_buffer`、`module.process`、`shell.socket`。
  Linux／macOS 此輪未建置或執行。

<a id="2026-10-04-可讀性收尾第二輪"></a>

### Readability closeout, stage 2 (2026-10-04) / 2026-10-04 可讀性收尾：第二輪

- Pipe-stream concepts were expanded; six named cases cover streaming/EOF, independent directions, capacity/Reader ownership, cancellation/timeout/backpressure, Writer move/close, repeated lifecycle/cleanup. Resource scopes, assertions, and ordering remain.
- Tests/helpers now name wait limits, short probes, transfer blocks, and repetition counts with unchanged values. Data/errors/exit codes/CTest timeouts remain; test constants do not enter framework config.
- README/SHARED_MEMORY examples expand branches and name wait/mapping values.
- Same Windows toolchain: full Debug/Release builds and **8/8** each (seven osal.*, sdk.consumer). Constant expansion/helper comparison checked operation/data/assertion order; production implementation/API unchanged. Snippets were manually checked, not separately compiled. No Linux/macOS run.

- `tests/pipe_stream_tests.cpp` 展開 concepts，將原本 main 內的六組情境拆成具名函式：
  串流與 EOF、獨立方向、容量與 Reader 所有權、取消／逾時／背壓、Writer 移動與關閉、
  重複生命週期與回收。各情境的資源作用域、斷言及執行順序保留。
- OSAL 測試及 helper 的等待上限、短逾時探測、主要傳輸區塊與重複次數改用用途明確的
  測試常數；保持原值。測試資料、預期錯誤、退出碼與 CTest 逾時設定保持原樣。
  測試常數留在測試內，不加入框架系統設定。
- 本文件及 `SHARED_MEMORY.md` 展開範例分支，為等待時間與映射容量命名。
- 同第一輪 Windows 工具鏈：Debug／Release 完整建置成功，七個 `osal.*` 套件及
  `sdk.consumer` 兩種組態各 **8/8** 通過。修改前後的操作、測試資料與斷言順序，
  另經展開常數及函式拆分的比對確認；正式 OSAL 實作及公開介面本輪未修改。
  文件片段經人工核對，未新增獨立的片段編譯測試。Linux／macOS 未執行。

<a id="複掃缺口修正與驗證2026-10-04"></a>

## Follow-up scan fixes and verification (2026-10-04) / 複掃缺口修正與驗證（2026-10-04）

- Readable/Writable concepts were expanded again with a no-I/O compile-time explanation; the same clang-format configuration retained multiline constraints/assertions.
- Eight casts were checked: seven gained representation/aliasing/lifetime explanations. FileLock test path_from now constructs a real u8string through iterators. char may inspect object representations, but char8_t lacks the same aliasing exception; see [C++ type access](https://eel.is/c++draft/basic.lval#11). Conversion preserves UTF-8 bytes, including high bits, without transcoding.
- Same Windows toolchain: full Debug/Release builds, **13/13** tests each (seven osal.*, six shell.*), including cross-process Unicode FileLock paths. Snapshot comparison found only formatting/comments beyond that character conversion. No Linux/macOS run. The old Shell conversion concern was subsequently addressed by UTF-8 helpers; see the path section above.

- `tests/pipe_stream_tests.cpp` 的 `Readable`／`Writable` 已再次展開，並補上編譯期
  檢查不執行 I/O 的說明。使用前輪同一份 clang-format 設定重新排版後仍維持多行；
  約束及 static assertions 保持原樣。
- 前次列出的八處轉型已核對：七處補上原生表示、別名存取與生命週期說明；
  `tests/file_lock_tests.cpp::path_from()` 改由 iterator 建立真正的 `std::u8string`。
  `char` 可讀取其他型別的物件表示，但 `char8_t` 不具備同樣的別名存取例外，
  因此不能直接反向轉型後讀取；依據見 [C++ 型別存取規則](https://eel.is/c++draft/basic.lval#11)。
  新轉換逐字元保留 UTF-8 位元組，包含高位元為 1 的值，不做編碼轉譯。
- Windows x64 同前輪 MSVC 工具鏈：Debug／Release 完整建置成功；七個 `osal.*`
  與六個 `shell.*` 測試兩種組態各 **13/13** 通過，包含 FileLock 既有的跨程序中文路徑。
  另以修改前快照比對，確認除上述字元轉換外僅有排版與註解差異。
  Linux／macOS 未執行；當時的 Shell 路徑轉型問題後續已透過 UTF-8 helper 處理，見本頁路徑章節。

<a id="待處理事項"></a>

## Outstanding issues and resolved findings / 待處理事項與已解決發現

- **Descriptor rollback fixture, resolved (2026-10-27):** On Ubuntu 24.04 / WSL 2, GCC 13.3 and CMake 3.28.3, the original `osal.descriptor` passed directly but failed under CTest. `strace` confirmed that CTest's log occupied inherited fd 3; the fixture obtained fd 4 and rejected it before setting the resource limit or exercising rollback. The fixture now limits new descriptor numbers to `filler + 1`, verifies non-stdio exhaustion with `EMFILE`, and explicitly checks limit restoration. Anonymous shared diagnostics preserve child assertions without consuming a descriptor. Additional cases inherit occupied low descriptors and a sparse high descriptor. Direct execution, an explicit inherited-fd-3 launcher, and CTest passed; the descriptor test also passed 20 consecutive CTest runs. Related Linux descriptor/Process/SharedMemory/FileLock tests passed **4/4**. Tracing confirmed low-fd acquisition, relocation failure, close/unlink of new resources, and preservation of existing names after failed open. Five isolated negative controls (the old fixture assumption, omitted FileLock/SharedMemory create rollback, and erroneous unlink on either open failure) all failed as expected; restoring the copied sources passed. Production OSAL code was not changed. This validates these rollback paths, not all Linux behavior or the separately deferred creation/spawn race. Existing macOS validation remains recorded in the [validated environment](../../../README.md#macos-validation); this fixture revision was exercised on Ubuntu 24.04.
- **Descriptor 回滾測試準備邏輯，已修正（2026-10-27）：** Ubuntu 24.04／WSL 2、GCC 13.3、CMake 3.28.3 上，原版直接執行通過，CTest 則失敗。`strace` 確認 CTest 日誌占用繼承的 fd 3，測試取得 fd 4 後，在設定資源上限及驗證回滾之前就誤判失敗。現在依實際 `filler + 1` 設限，明確驗證非標準串流 fd 已耗盡並回報 `EMFILE`，也檢查上限還原是否成功。匿名共享診斷保留子程序 assertion，且不占用 fd；新增繼承低編號與不連續高編號 fd 的案例。直接執行、明確繼承 fd 3、CTest 均通過，descriptor 另連跑 20 次通過；Linux descriptor／Process／SharedMemory／FileLock 共 **4/4** 通過。追蹤確認低 fd 建立、搬移失敗、新資源 close／unlink，以及 open 失敗保留既有名稱。隔離副本的五個負向對照（舊 fixture 假設、兩種 create 回滾缺漏、兩種 open 誤刪名稱）均被測試攔下，還原後通過。正式 OSAL 未修改；本次結論限於這些路徑，不代表所有 Linux 行為或另列的建立／spawn 競爭窗口已驗證。macOS 已有的驗證結果見[已驗證環境](../../../README.md#macos-validation)；本次 fixture 修訂於 Ubuntu 24.04 執行驗證。

- **Pipe SIGPIPE and submission setup errors (2026-10-27):** macOS-managed Pipe I/O uses F_SETNOSIGPIPE before admission; setup failure now retains ReadPipe/WritePipe outside AdoptPipe. Native child stdio keeps its existing policy. Cross-platform contract tests and deterministic scheduler fault tests are registered with CTest. See [Pipe](PIPE.md) for macOS and Windows results and the remaining Linux Pipe-specific coverage; synthetic setup failure does not establish native fcntl/IOCP failure coverage.
- **Pipe 斷管與提交錯誤（2026-10-27）：** macOS 在接受 Pipe I/O 前設定 F_SETNOSIGPIPE；設定失敗的公開 operation 已修正為 ReadPipe／WritePipe，原因鏈保留 AdoptPipe。原生子程序 stdio 保留既有政策。CTest 已加入共同契約及 scheduler 故障測試；macOS／Windows 結果、Linux Pipe 專項的剩餘覆蓋與原生故障注入限制見 [Pipe](PIPE.md)。

- **FileLock ownership after native unlock failure (2026-10-05):** FileLockGuard::unlock retains the Guard/local occupied flag on backend failure, proving only retained cleanup responsibility, not continued native ownership. [UnlockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-unlockfileex), [Linux flock](https://man7.org/linux/man-pages/man2/flock.2.html), and [macOS flock](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/flock.2.html) do not guarantee unchanged ownership for every failure. Retryable Guard state alone does not establish safe payload access. No failure-after-release case has been reproduced and no revised policy is approved. The user deferred implementation; [root TODO](../../../TODO.md) tracks it. Since 2026-10-08 Concurrency no longer uses FileLock, but this OSAL gap remains. Decide known/unknown ownership, cleanup, retry, and fault policy before changes; the superseded Concurrency refactor did not fix FileLock.
- **POSIX internal-fd/stdio isolation:** Linux/macOS monitors, POSIX FileLock/SharedMemory originally could retain fd 0/1/2 after the host closed stdio. Spawn Inherit preserves these via adddup2(fd, fd), which on Linux clears CLOEXEC ([glibc](https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/spawni.c)); CLOEXEC alone was insufficient. A child could write internal resources or retain a FileLock open description after owner death. The implemented descriptor helper relocates these resources and Linux pidfd before delivery/registration; failure preserves native errors and closes fds. Name rollback removes only new undistributed resources, not opened ones. POSIX osal.descriptor covers closed stdio, mapping/locking, child Null streams, exhausted-fd rollback/retry, and no child-extended ownership. For current validation, see the descriptor fixture findings above for Ubuntu 24.04 / WSL 2 and the [existing macOS report](../../../README.md#macos-validation). Relocation addresses retained occupancy, not the separate concurrent window below.
- **Deferred POSIX concurrent creation/spawn:** relocation is not one native operation. Another thread can spawn after open/pipe/socket receives a low fd but before relocation, causing accidental inheritance or invalid-source launch failure. Pipe/Socket and newly covered resources all remain exposed. Interleavings are identified in code but not reproduced with controlled scheduling or fixed; the user deferred changes and [TODO](../../../TODO.md) retains tracking. Resuming requires scope confirmation/authorization. **Unapproved proposal:** one private process-local mutex around fd creation, relocation, and rollback; under the same mutex spawn duplicates inherited streams outside Pipe bootstrap fd ranges and records missing streams as /dev/null, then releases the lock before spawn, using RAII copies. This must cover Pipe, Socket/accept, FileLock, SharedMemory, monitors, and pidfd, not just relocation. Decisions include lock contention and up to three temporary fds per spawn, and whether protection covers only directly managed OSAL resources. Third-party open/close/dup2 and libc resolver-created sockets are uncoordinated. Filling missing parent stdio with /dev/null during single-thread startup is another unapproved alternative because it mutates host state. Future Linux/macOS tests must pause after low-fd acquisition, interleave spawn, check Inherit/Null/Capture, exhaustion/rollback, lock order/deadlock. Linux VM/WSL2 and Mac/macOS CI can run separately; Windows regressions cannot replace POSIX race tests. There is no measured fix result.
- **Windows Pipe random diagnostics, resolved:** NativePipe::create_pair previously discarded BCrypt NTSTATUS. It now shares random_bytes and wraps CreatePipe ← RandomBytes with native data. Windows osal.pipe_random_failure replaces the provider at link time and exercises real PipeService creation, repeated errors, admission recovery, and later success. No production test hook or real BCrypt fault injection was added.

- **FileLock 原生解鎖失敗後的持鎖狀態（2026-10-05 Concurrency 重審）：**
  `src/file_lock.cpp::FileLockGuard::unlock()` 於後端失敗時保留 guard 與本地 occupied，
  這只能證明本地清理責任尚未交卸，不能作為 native lock 仍被持有的查證。
  [UnlockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-unlockfileex)、
  [Linux flock](https://man7.org/linux/man-pages/man2/flock.2.html) 與
  [macOS flock](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/flock.2.html)
  文件未提供涵蓋全部失敗的持鎖不變保證。現有 FILE_LOCK.md 的「失敗保留 guard，可重試」
  不足以定義資料存取是否仍安全；尚未重現原生失敗後已解鎖的情境，也未決定修訂契約。
  **狀態（2026-10-05）：使用者決定先記錄，暫不處理。** 根目錄 [TODO](../../../TODO.md)
  保留追蹤入口。2026-10-08 Concurrency 已改用 SharedMemory／SharedWord，
  不再使用 FileLock；本項仍是 OSAL FileLock 自身的待辦。
  **下一步：** 區分已知狀態與無法確認狀態，先與使用者確認清理／重試／故障政策，
  再修改 OSAL。舊 Concurrency 解鎖重構提案已由新共享鎖取代，不代表 FileLock 缺口已解決。
- **POSIX 內部 fd 與標準串流隔離不一致（2026-10-05 重審）：**
  `src/linux/monitor.cpp`、`src/macos/monitor.cpp`、`src/posix/file_lock.cpp` 與
  `src/posix/shared_memory.cpp` 原先取得原生 fd 後，未避開標準串流使用的 0／1／2。
  若宿主先關閉標準串流，這些內部資源可能取得空出的編號；`src/posix/process.cpp`
  的 Inherit 路徑會把它們當成標準串流，以 `adddup2(fd, fd)` 保留到子程序。
  Linux 的該 spawn action 會清除 `FD_CLOEXEC`，因此只加 CLOEXEC 不足以防止誤繼承
  （[glibc 實作](https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/spawni.c)）。
  可能造成子程序輸出寫入內部資源；FileLock 的 open file description 若被繼承，
  也可能在原持有者異常退出後繼續維持占用。
  **修正狀態：** 已加入內部 `detail/posix/descriptor.hpp`，以上後端及 Linux pidfd
  在交付／登記事件前搬至非標準串流編號；失敗保留原生錯誤並回收 fd。
  FileLock／SharedMemory 的搬移失敗只移除本次新建且尚未交付的名稱，不移除 open 的既有資源。
  新增 POSIX 專用 `osal.descriptor`：關閉標準串流後建立／開啟資源、映射與鎖仍可使用、
  子程序標準串流改接空裝置、fd 額滿時回滾及同名重試、持鎖者直接退出後子程序不延長占用。
  **目前驗證：** Ubuntu 24.04／WSL 2 的 descriptor 與相關原生測試已通過，詳見上方 fixture 修正紀錄；macOS 既有建置與測試結果見[已驗證環境](../../../README.md#macos-validation)。
  此修正處理資源建立完成後的持續占用，並行建立／spawn 的窗口另見下項。
- **POSIX 關閉標準串流時的並行建立／spawn（2026-10-05 新發現）：**
  搬移 fd 不是單一原生操作。另一執行緒可能在 open／pipe／socket 等取得低編號 fd 後、
  搬移之前，進入 `src/posix/process.cpp` 的標準串流檢查及 spawn。
  既有 Pipe／Socket 和本輪補齊的資源均不能僅靠事後搬移宣稱完全排除此窗口。
  **影響：** 子程序可能誤繼承內部資源，或在 fd 搬移／關閉後因來源失效而啟動失敗。
  **狀態（2026-10-05）：使用者決定暫緩處理。** 程式交錯路徑確認，尚未用受控排程重現或修復；
  未擴大修改同步邊界。根目錄 [TODO](../../../TODO.md) 保留追蹤入口；恢復工作須重新確認範圍並取得開工授權。
  **未核准提案：** OSAL 私有程序內 mutex 保護原生 fd 建立、搬離 0／1／2 及失敗清理的完整區間；
  spawn 在同一把鎖內複製需繼承的標準串流，副本避開 Pipe bootstrap 目標編號區間，
  不存在的串流記錄為子程序接 `/dev/null`。快照完成後釋放鎖，再執行 spawn；副本由 RAII 回收。
  需涵蓋 Pipe、Socket／accept、FileLock、SharedMemory、各監看器及 pidfd，不能只鎖搬移 helper。
  **待決策：** 是否接受建立／accept 與快照的鎖競爭，以及每次 spawn 最多三個暫存 fd 的成本；
  是否將保證限定於 OSAL 直接管理的資源。宿主／第三方的 open、close、dup2 不受私有鎖協調，
  libc resolver 也可能自行建立 socket，不能宣稱所有 OSAL 呼叫涉及的間接 fd 都已受保護。
  宿主於單執行緒啟動階段將缺少的標準串流接到 `/dev/null` 並維持有效，是另一個待討論方向；
  此舉會改變父程序狀態，尚未核准由 OSAL 或 Vinc 入口自動執行。
  **恢復後驗證：** Linux、macOS 各自建置／執行，刻意停在取得低編號 fd、尚未搬移的位置，
  驗證與 spawn 的交錯、Inherit／Null／Capture 行為、fd 額滿及失敗回收、鎖順序與無死鎖。
  Linux 可使用 VM／WSL2，macOS 使用 Mac 或 macOS CI runner；不要求同時執行。
  Windows 僅能驗證共用程式回歸，不能取代 POSIX 競爭測試；目前尚無該修復的實測結果。
- **Windows Pipe 遺失亂數失敗的原生診斷（2026-10-05 重審）：**
  `src/windows/pipe.cpp::NativePipe::create_pair()` 原先直接呼叫 `BCryptGenRandom`，
  失敗僅回傳 `SystemError / CreatePipe`，沒有保留回傳的 NTSTATUS；
  `src/random.cpp::random_bytes()` 已保留 `NativeErrorSource::NtStatus` 與原始數值。
  **修正狀態：** 已共用 `random_bytes()`，以 CreatePipe 包裝 RandomBytes 原因，保留原生來源／數值。
  新增 Windows 專用 `osal.pipe_random_failure`，以連結時替換亂數 provider，走實際
  PipeService 建立路徑，驗證連續失敗的 NTSTATUS 傳遞及名額回收，恢復後可成功建立。
  不新增正式 runtime 測試 hook；此測試不是 BCrypt 本身的原生故障注入。

<a id="fd-隔離與-pipe-原生診斷修正驗證2026-10-05"></a>

### fd isolation and Pipe diagnostic validation (2026-10-05) / fd 隔離與 Pipe 原生診斷修正驗證（2026-10-05）

Windows x64/MSVC 19.51.36257, SDK 10.0.26100.0, CMake 4.3.2, Ninja 1.13.2: related Debug/Release builds passed; OSAL CTests **11/11** each (5.84 s / 4.86 s). Other components were not rerun; no Linux/macOS build/run. Public API, codes, and stdio Inherit/Null/Capture semantics were preserved.

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
相關 OSAL targets Debug／Release 建置成功，OSAL CTest 各 **11/11** 通過
（Debug 5.84 秒、Release 4.86 秒）。未重跑其他元件的測試；Linux／macOS 未建置或執行。
公開 API、錯誤碼編號及既有 stdio Inherit／Null／Capture 語意不變。

<a id="osal-操作原因鏈補齊與驗證2026-10-05"></a>

### Operation-cause integration and validation (2026-10-05) / OSAL 操作原因鏈補齊與驗證（2026-10-05）

- Integrated the Socket/Poller, Windows Pipe, Process+Pipe boundaries listed above, without new identifiers or wire changes.
- Production operation comparisons were scanned; no caller branched on the changed OSAL outer operation. Protocol operation fields are distinct. Test assertions follow the updated contract.
- osal.operation_error covers real Socket timeout/cancellation/recovery, direct Poller failures, spawn capacity/rollback, no redundant wrapping, unknown values, and bounded omission.
- Pipe random tests cover Spawn ← CreatePipe ← RandomBytes and admission recovery; osal.pipe checks AdoptPipe ← CreatePipe while retaining handoff/EOF. Not every Winsock/Poller native failure branch was injected; provider substitution is not a BCrypt fault.
- Windows x64/MSVC 19.51.36257, SDK 10.0.26100.0, CMake 4.3.2, Ninja 1.13.2: full Debug/Release builds; **31/31** related CTests each across OSAL, Control, Module, OutputHub, Concurrency, Shell, Bus, SDK. After final capacity-assertion edits, Debug reran **2/2** cause tests; Release 31/31 used final sources. No Linux/macOS build/run.
- Concurrent POSIX fd creation/spawn remains deferred above.

- 已補齊本文件「統一錯誤介面」列出的 Socket／Poller、Windows Pipe 及 Process＋Pipe
  組合邊界；未新增錯誤碼、操作碼或修改跨程序編碼。
- 已掃描正式程式的 operation 比較；目前沒有呼叫端依賴此次改變的 OSAL 外層 operation
  作分支。協定命令的 operation 欄位不屬於錯誤操作碼；原有測試斷言依新契約補齊。
- 新增 `osal.operation_error`：實際 Socket 逾時／取消及恢復收發、直接 Poller 錯誤、
  Spawn 容量失敗與回滾、同操作不重複包裝，以及未知值與固定容量原因鏈省略。
- `osal.pipe_random_failure` 補上 Spawn ← CreatePipe ← RandomBytes 與名額回收；
  `osal.pipe` 的子程序容量拒絕情境補驗 AdoptPipe ← CreatePipe，保留原有接手／EOF 回收驗證。
  Windows 原生 Winsock 初始化及 Poller 建立的每一個底層失敗分支並未逐一注入；
  亂數測試是連結時 provider 替換，不代表 BCrypt 原生故障已被注入。
- Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
  完整專案 Debug／Release 建置成功，OSAL 與 Control Channel、Module System、OutputHub、
  Concurrency、Shell、Message Bus、SDK 相關 CTest 兩種組態各 **31/31** 通過。
  收尾調整測試的原因鏈容量斷言後，Debug 另重跑兩個原因鏈測試 **2/2** 通過；
  Release 的 31/31 使用收尾版本。Linux／macOS 本輪未建置或執行。
- POSIX fd 並行建立／spawn 的同步問題未納入此輪，仍保留於上方待處理事項。

<a id="先前已處理事項"></a>

### Previously resolved issues / 先前已處理事項

**Native classification (2026-10-04):** Windows random.cpp previously placed BCryptGenRandom status in std::system_category, despite the [native contract](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom) specifying NTSTATUS. It now retains NativeErrorSource::NtStatus and raw 32-bit value with numeric fallback. Synthetic NTSTATUS/child transport tests passed; no real BCrypt failure was injected, so actual native failure coverage is not claimed.

- **原生錯誤分類（2026-10-04 review）：** `src/random.cpp` 的 Windows 失敗路徑將
  `BCryptGenRandom` 回傳值直接包成 `std::system_category()`。Microsoft 文件明確將
  回傳值定義為 `NTSTATUS`，目前未保留它與 Win32 錯誤碼的分類差異，診斷文字可能被
  錯誤解讀。來源：[BCryptGenRandom](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom)。
  **處理狀態：** 本輪已改為 `NativeErrorSource::NtStatus` 與原始 32-bit 數值，採數值
  fallback；新增合成 NTSTATUS 及跨程序傳遞測試。沒有注入真正的 BCryptGenRandom 故障，
  不宣稱已驗證該 API 的實際失敗流程。此項完成的測試結果見本輪驗證紀錄。

<a id="dynamiclibrary2026-10-06"></a>

## DynamicLibrary（2026-10-06） / DynamicLibrary (2026-10-06)

`<vinc/osal/dynamic_library.hpp>` provides a move-only native loader reference. open(absolute_path), function(name), close() return Result; destruction is best-effort close. Symbols last only while the library lives; callers restore the correct signature and stop borrowing work before release. This general API has no ModuleType, Plugin ABI, RuntimeServices, or loading deadline policy.

`<vinc/osal/dynamic_library.hpp>` 提供 move-only native loader reference：
`open(absolute_path)`、`function(name)`、`close()` 均使用 Result，析構 best-effort close。
符號只在庫存活期間有效；呼叫端須轉回正確函式簽章，並在釋放庫前停止所有借用程式碼的工作。
此通用介面沒有 ModuleType、Plugin ABI、RuntimeServices 或載入時限政策。

Windows uses LoadLibraryExW with the DLL directory and OS default search directories. POSIX uses RTLD_NOW/RTLD_LOCAL, with dependency lookup determined by its loader. Primary paths must be absolute without NUL; symbols nonempty without NUL. Loading/unloading can execute user code and block; no cancellation, no-allocation, or no-exception guarantee is given. Failed close retains the handle for explicit retry.

Windows 使用 LoadLibraryExW，以 DLL 目錄及 OS default search directories 解析相依；
POSIX 使用 RTLD_NOW／RTLD_LOCAL，相依庫位置依該平台的 loader 設定。Primary path 必須
絕對且不含 NUL，符號名稱非空且無 NUL。載入／卸載可執行使用者程式碼，可能阻塞，
OSAL 不承諾可取消、無配置或無例外。close 失敗時保留 handle，供明確重試。

Operations OpenLibrary=46, FindSymbol=47, CloseLibrary=48 were appended. Windows retains GetLastError. POSIX dlerror is text rather than errno; currently retain only portable classification/operation, without inventing native numeric codes. Detailed POSIX loader text remains a design limitation; Common Error layout is unchanged.

新增操作碼 OpenLibrary=46、FindSymbol=47、CloseLibrary=48；既有代碼不變。
Windows 保留 GetLastError；POSIX dlerror 是文字而非 errno，目前僅保留操作與通用錯誤分類，
不偽造 native 數字。詳細 POSIX loader 診斷是尚待設計的限制；本輪不改 Common Error 布局。

Recorded Windows coverage comes from Module System module.plugin: full Debug/Release suites passed **55/55** each, including actual load/symbols, move/close, Chinese/spaced paths. For later platform evidence, see [Current platform validation](#current-platform-validation) and [Module System](../module_system/README.md).

本輪 Windows DynamicLibrary 行為由 Module System 的 `module.plugin` 測試涵蓋，
Debug／Release 全套各 55/55 通過；包含實際載入／符號、move／close、中文／空白路徑。
後續平台結果見[目前平台驗證](#current-platform-validation)，載入範圍與限制見 [Module System](../module_system/README.md)。

<a id="notification-validation"></a>

## Notification validation (2026-10-28) / 通知功能驗證

The [Notification contract](NOTIFICATION.md) owns the API, native mechanisms,
lifetime, cancellation, and remaining limitations. This round implements only
the OSAL primitive and tests. Bus publication and `Context::pumpLoop()` do not
use it yet; no claim is made that the earlier approximately 16 ms tail latency
has been fixed.

[通知契約](NOTIFICATION.md)維護 API、後端、生命週期、取消與限制。
本輪只完成 OSAL 原語與測試，Bus／Context 尚未接線，不能宣稱已改善約 16 ms 尾延遲。

| Environment / 環境 | Results / 結果 |
| --- | --- |
| Windows x64, MSVC 19.51.36257, Debug | OSAL **19/19**, plus SDK consumer **1/1** passed. |
| Windows x64, MSVC 19.51.36257, Release | OSAL **19/19** passed; notification test also passed **20 consecutive runs**. |
| WSL2 Ubuntu 24.04.5 x86_64, GCC/libstdc++ 13.3.0, Debug | Standalone OSAL **24/24** passed; both notification tests also passed **20 consecutive runs each**. |
| macOS | Backend and tests added; native execution of this new functionality is pending. Prior macOS results for other OSAL facilities remain valid. |

**Receiver-open follow-up (2026-10-28):** Added `NotificationReceiver::open()`
without creating/resetting resources or changing error numbers. Windows
Debug/Release OSAL **19/19 each** and WSL Ubuntu 24.04 Debug **24/24** passed
again with receiver-open tests. These cover actual cross-process handoff,
pre-open hints, cancellation after creator close, and failed opens preserving
resources/data. The SDK consumer and 20-run records above belong to the earlier
primitive pass; they were not rerun for this addition. New macOS behavior remains
pending native validation. That OSAL-only pass did not change central/Host/Bus/pump
wiring; the subsequent central/Host phase is recorded in [Module ownership](../module_system/INSTANCE.md).

**接收端 open 補充驗證（2026-10-28）：** 新增只開啟、不建立或清空資源的 API，錯誤編號不變。
Windows Debug／Release OSAL 再次各 **19/19**、WSL Ubuntu 24.04 Debug **24/24** 通過，
包含真正跨程序交接、保留先到提示、建立者關閉後取消，以及開啟失敗時保留資源與資料。
上表 SDK consumer 與連跑 20 次是前一輪原語的紀錄，本次未重跑；新增功能的 macOS
原生驗證仍待補。該輪未改中央、Host、Bus 與 pump；後續中央／Host 接線見
[Module 資源所有權](../module_system/INSTANCE.md)。

The full notification test includes real child processes, concurrent senders,
indefinite-wait cancellation, full-buffer coalescing, killed-receiver behavior,
native resource failures and cleanup. On Windows the lifecycle handle-count
probe runs before ProcessService starts asynchronous reaping, avoiding unrelated
handle-count changes. POSIX native fault cases run in isolated children and
preserve inherited descriptors; no fd-3-is-free assumption is introduced.

驗證含真正的子程序、並行發送、無限等待取消、滿載合併、接收程序死亡與原生失敗回收。
Windows 的 handle 計數先於 ProcessService 啟動，避免非同步回收干擾基準；
POSIX 故障案例在隔離子程序執行，保留繼承 FD，不假設 fd 3 必須空閒。
新增通知功能的 macOS 驗證待執行，沒有撤銷其他元件已有的 macOS 驗證結果。

Reproduce after building the matching targets / 建置後執行：

```powershell
ctest --test-dir build/windows-msvc-debug -R "^osal\." --output-on-failure
ctest --test-dir build/windows-msvc-release -R "^osal\." --output-on-failure
ctest --test-dir build/windows-msvc-release -R "^osal\.notification$" --repeat until-fail:20 --output-on-failure
```

```sh
ctest --test-dir /home/vinc-test/build/osal-debug -R '^osal\.' --output-on-failure
ctest --test-dir /home/vinc-test/build/osal-debug -R '^osal\.notification' --repeat until-fail:20 --output-on-failure
```

# OSAL Process

OSAL 提供同一份 C++23 程序操作介面。框架只使用
`<vinc/osal/process.hpp>` 並連結 `vinc::osal`；CMake 依目標 OS 選擇後端。
公開標頭不包含原生程序型別或平台條件編譯。

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

## 標頭與實作位置

公開標頭維持 `<vinc/osal/...>`。native、state 與平台內部標頭統一放在 `include/vinc/osal/detail/`；
`src/` 保留共用及各平台的 .cpp。detail 僅供本元件實作與指定測試使用，
元件目錄整理不改變平台行為契約；本輪錯誤型別遷移見下節。

## 統一錯誤介面

OSAL 使用 `vinc::Result<T>`／`vinc::Error`，不提供獨立 Result 或 Error payload。
`<vinc/osal/error.hpp>` 定義 `vinc::osal::ErrorCode` 與 `vinc::osal::ErrorOperation`，
對應 common 的 `ErrorDomain::Osal`；無回傳資料的操作使用
`vinc::Result<void>`。錯誤與結果直接引用 `<vinc/result.hpp>`；截止時間使用
`<vinc/time.hpp>` 的 `vinc::Deadline`，即 `std::chrono::steady_clock::time_point`。
舊的錯誤轉接標頭已由本元件 enum 定義取代。Deadline 是本地等待的截止時間，不是日期時間，
也不直接序列化為跨程序協定欄位；原有等待、取消與逾時行為保持不變。

`Error` 的 `domain`、`code`、`operation`、`native` 保存分類、失敗操作與原生來源／數值。
使用 `error.is(osal::ErrorCode::Timeout)` 等具 domain 的比較，文字表由 OSAL 自行維護。
`vinc::error_to_string(error)` 統一產生診斷文字；原生錯誤沒有值時，不輸出「成功」之類
容易誤導的 OS 訊息。未知錯誤／操作保留數值，程式判斷仍使用具型別的錯誤碼。
目前有 23 個錯誤碼與 45 個非零操作碼；既有明確指定的編號保持不變，domain 為完整身分的一部分。
它們不相容於更早期 enum 的隱含序號；C++ Error 布局仍不是跨版本傳輸協定。
現有逾時、取消、生命週期、平台錯誤分類與例外邊界保持原樣。

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

已接線的組合邊界：

- Socket 的 accept／connect／send／receive 等 `*_until()` 保留其公開操作，
  下層 Poller 的初始化、登記、等待或喚醒失敗保留於原因鏈；直接呼叫 Poller 仍回報 Poller 操作。
- Socket 建立、accept 後的原生設定、connect 的 peer 查詢、hostname 解析及 Poller 建立，
  均保留當前 API 操作與下層失敗步驟。
- `spawn_with_pipe()`、`spawn_with_pipes()`、`spawn_with_output()` 的前置資源失敗以 Spawn 回報；
  繼承端點接手失敗以 AdoptPipe 回報。直接的 Spawn／AdoptPipe 失敗不再增加同名原因。
- Windows Pipe 亂數失敗為 CreatePipe ← RandomBytes；經程序建立便利 API 回報時，
  為 Spawn ← CreatePipe ← RandomBytes（依設定容量保留或標記省略）。

這些變更不改變 Timeout／Cancelled 等錯誤碼、等待順序、取消結果、回收及例外邊界。
呼叫端以目前層作程式判斷；原因鏈供診斷，不需依賴內部失敗步驟來控制流程。

本輪驗證（2026-10-04）：Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、
CMake 4.3.2、Ninja 1.13.2。完整 Debug／Release 建置及公開標頭編譯成功，全套 CTest
各 **30/30** 通過；獨立 OSAL Release 從乾淨目錄建置，common 與 OSAL 測試 **11/11** 通過。
獨立 common Release 無 OSAL provider 的建置與 **2/2** 測試亦通過。新增驗證包括靜態表格
與 CMake 接線的負向案例，以及子程序退出後，由父程序解讀共享記憶體中的錯誤欄位。
該編碼是測試專用格式，沒有修改 Module／Message Bus 的正式協定。
原生 NTSTATUS 保留來源與數值，測試使用合成值，未注入 BCryptGenRandom 的實際失敗。
Linux／macOS 已更新來源，但尚未建置或執行驗證。


## UTF-8 與原生路徑

`<vinc/osal/path.hpp>` 提供 `path_from_utf8(std::string_view)` 與
`path_to_utf8(const std::filesystem::path&)`，分別回傳 `Result<std::filesystem::path>`
及 `Result<std::string>`，連結既有 `vinc::osal`；SDK 整合入口亦包含此標頭。

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

### 路徑轉換驗證（2026-10-05）

新增 `osal.path`：中文／emoji 往返、實際開檔、空值、相對路徑與前綴保留、非法 UTF-8、
Big5 bytes、NUL、無法輸出的原生編碼。Windows 案例含孤立 UTF-16 surrogate；
POSIX 案例含非法原生 bytes 與合法反斜線檔名。SDK 每個標頭獨立編譯亦涵蓋 path.hpp。
Shell 整合另驗證本機／遠端 Unicode 腳本、Unicode 部署目錄與 OSAL 原因鏈。
Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 完整建置與公開標頭編譯成功；相關 CTest 各 20/20 通過
（Debug 34.80 秒、Release 34.44 秒）。Linux／macOS 尚未實機驗證。
未注入原生轉換 API 的配置故障，亦未配置超過 INT_MAX 的測試字串。

## 統一操作契約

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

`terminate()` 回傳 `Requested` 表示 OS 已接受要求，**不表示退出已完成**；
接著使用 `wait_until()` 確認。已觀察到退出時回傳 `AlreadyExited`。
逾時或取消不會丟失管理權；中央仍可終止、等待與查詢。所有期限使用
`steady_clock`。原生啟動呼叫本身沒有可取消或硬即時期限的保證。

`spawn()` 成功表示子程序已建立並納入管理，不表示模組初始化完成。
原生啟動失敗以錯誤回報；POSIX 若以新程序退出碼呈現後續啟動失敗，則保留
該退出碼，不把 127 一律解讀成某一種錯誤。

## 所有權與回收

**觀察失敗與控制能力（2026-10-05 修正）。**

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

新增 `osal.process_faults` 以測試 native backend 驗證觀察失敗、終止、恢復、
診斷保留與失去所有權；真實子程序另由既有 process／process_output 與 Module 測試驗證。
Windows Debug／Release 相關測試各 9/9 通過，完整項目與限制見
[Module System 本輪驗證](../module_system/README.md#本輪驗證結果)。

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

同一個存活的服務與控制物件允許並行呼叫一般操作；同一紀錄的原生等待、
回收與終止會序列化。移動、解構物件，以及修改傳入的 `SpawnOptions`，
必須由呼叫端與其他使用者同步。關閉前已接受的 `spawn()` 可以完成；
關閉後才嘗試進入的建立操作回報 `ServiceClosed`。

目前以靜態庫整合，不提供執行中卸載 OSAL 動態庫的契約。
`shutdown_until()` 確認程序觀察已完成，並非動態庫安全卸載屏障。

## 啟動參數與錯誤

- `SpawnOptions::new_process_group` 預設 `false`；ModuleManager 啟動模組時設為 `true`。
  Windows 建立新群組並停用子程序的 CTRL_C_EVENT；POSIX 使用子程序 PID 建立新程序群組，
  與父程序的終端前景群組分離。目的為讓中央協調關閉；不建立新 console／session，
  `terminate()` 仍只處理直接管理的程序。Windows CTRL_BREAK_EVENT 及對子程序個別送出訊號不在此隔離保證內。
  平台語意見 [Windows 建立旗標](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags)
  與 [POSIX spawn](https://man7.org/linux/man-pages/man3/posix_spawn.3.html)。

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

## 平台後端與限制

以下是 OSAL 內部實作，框架呼叫端使用上面的同一套介面。

| 平台 | 建立 | 退出通知與回收 | 強制終止 |
| --- | --- | --- | --- |
| Windows | `CreateProcessW`，明確 handle 清單 | thread-pool wait 通知監看執行緒；等待 process handle 並讀取退出碼 | `TerminateProcess` |
| Linux | `posix_spawn`，關閉非標準 fd | pidfd + epoll 通知；`waitpid` 回收 | `pidfd_send_signal(SIGKILL)`；未取得 pidfd 時使用受管理的子程序 PID |
| macOS | `posix_spawn`，`POSIX_SPAWN_CLOEXEC_DEFAULT` | kqueue `NOTE_EXIT` 通知；`waitpid` 回收 | 對受管理的子程序送出 `SIGKILL` |

Linux／macOS 事件註冊失敗時，以 50ms 間隔的 `waitpid(WNOHANG)` 保留回收責任。
整個事件監看器失敗時也會降級；`monitor_error()` 保存這種基礎設施錯誤。
一般事件通知每次喚醒會掃描有界的管理紀錄，尚未提供大量程序的效能量測。

POSIX 嵌入條件：

- 不可將 `SIGCHLD` 設為 `SIG_IGN` 或啟用 `SA_NOCLDWAIT`；建立時會檢查。
  不可由其他 handler／執行緒對 OSAL 擁有的子程序呼叫 `wait*()`。
  其他元件使用 `waitpid(-1, ...)` 可能搶先回收 OSAL 子程序。
- OSAL 不改寫父程序全域 signal handler；子程序清除 signal mask，並恢復
  `SIGPIPE` 預設動作。其他原生忽略訊號的繼承依 POSIX 行為保留。
- 外部回收導致 `ECHILD` 時回報 `ObservationLost`，停止使用該 PID 控制程序，
  不假造成功退出。呼叫端需處理原因，再明確釋放紀錄。
- 繼承環境時宿主不得同時修改全域 `environ`；並行啟動不同環境應使用各自
  的 `SpawnOptions.environment` 或 `Replace`。

Linux 目前要求 glibc 的 `posix_spawn_file_actions_addclosefrom_np`（2.34 起），
符合 Ubuntu 22.04 的 libc 範圍；CMake 會檢查。macOS 會檢查所需 spawn 擴充。
仍須使用支援 `std::expected`、stop-token 條件等待等功能的 C++23 工具鏈。
Windows／macOS 最低部署版本尚未確立。

原生契約參考：[Microsoft CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)、
[POSIX spawn / Linux man-pages](https://man7.org/linux/man-pages/man3/posix_spawn.3.html)、
[Apple posix_spawn](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/posix_spawn.2.html)、
[glibc closefrom 擴充](https://sourceware.org/pipermail/glibc-cvs/2021q3/073654.html)。

## 使用範例

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

中央透過 `ProcessService::create()` 建立並長期持有服務及 ID，處理退出
結果後呼叫 `forget(id)`。終止失敗或等待逾時後，仍可使用相同 ID 繼續處理。
全部程序處理完畢後，以 `shutdown_until()` 完成正常關閉流程。

## 建置與驗證

OSAL 可單獨建置，只需一併提供相鄰的 `src/vinc/common`；不依賴 message generator
或其他 VINC 執行元件。`vinc::osal` 以 PUBLIC 相依傳遞 `vinc::common`，消費端只需連結
OSAL target。若上層尚未提供 `vinc::common`，獨立入口會自動加入旁邊的 common：

```sh
cmake -S src/vinc/osal -B build/osal -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/osal
ctest --test-dir build/osal --output-on-failure
```

Windows 在 x64 MSVC developer shell 執行，可加 `-DCMAKE_CXX_COMPILER=cl`；
全程為命令列 CMake，不需要 Visual Studio GUI。完整專案亦包含此 target
及 CTest；`-DBUILD_TESTING=OFF` 可停用測試程式。

測試包含立即退出、保存結果、強制終止、多等待者、逾時／取消、控制物件
釋放後繼續管理、Unicode 與引號參數、工作目錄、環境替換、標準串流、
額外 handle／fd 不被繼承、異常退出、並行建立、重複回收、容量及關閉流程。
Windows 另測試退出碼 259；POSIX 檢查子程序已被回收。

2026-09-18 驗證環境：Windows x64、MSVC 19.44、Windows SDK 10.0.26100、
CMake 3.31.6、Ninja 1.12.1。Linux 與 macOS 後端目前尚未在目標 OS 實際
編譯／執行；程式碼存在不代表三平台驗證完成。

### 2026-10-04 可讀性收尾：第一輪

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

### 2026-10-04 可讀性收尾：第二輪

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

## 複掃缺口修正與驗證（2026-10-04）

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
  Linux／macOS 未執行；Shell 其他路徑轉型仍見其 README 待辦。

## 待處理事項

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
  **驗證限制：** Linux／macOS 測試尚未編譯或執行；目前主機沒有可用 WSL。
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

### fd 隔離與 Pipe 原生診斷修正驗證（2026-10-05）

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
相關 OSAL targets Debug／Release 建置成功，OSAL CTest 各 **11/11** 通過
（Debug 5.84 秒、Release 4.86 秒）。未重跑其他元件的測試；Linux／macOS 未建置或執行。
公開 API、錯誤碼編號及既有 stdio Inherit／Null／Capture 語意不變。

### OSAL 操作原因鏈補齊與驗證（2026-10-05）

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

### 先前已處理事項

- **原生錯誤分類（2026-10-04 review）：** `src/random.cpp` 的 Windows 失敗路徑將
  `BCryptGenRandom` 回傳值直接包成 `std::system_category()`。Microsoft 文件明確將
  回傳值定義為 `NTSTATUS`，目前未保留它與 Win32 錯誤碼的分類差異，診斷文字可能被
  錯誤解讀。來源：[BCryptGenRandom](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom)。
  **處理狀態：** 本輪已改為 `NativeErrorSource::NtStatus` 與原始 32-bit 數值，採數值
  fallback；新增合成 NTSTATUS 及跨程序傳遞測試。沒有注入真正的 BCryptGenRandom 故障，
  不宣稱已驗證該 API 的實際失敗流程。此項完成的測試結果見本輪驗證紀錄。

## DynamicLibrary（2026-10-06）

`<vinc/osal/dynamic_library.hpp>` 提供 move-only native loader reference：
`open(absolute_path)`、`function(name)`、`close()` 均使用 Result，析構 best-effort close。
符號只在庫存活期間有效；呼叫端須轉回正確函式簽章，並在釋放庫前停止所有借用程式碼的工作。
此通用介面沒有 ModuleType、Plugin ABI、RuntimeServices 或載入時限政策。

Windows 使用 LoadLibraryExW，以 DLL 目錄及 OS default search directories 解析相依；
POSIX 使用 RTLD_NOW／RTLD_LOCAL，相依庫位置依該平台的 loader 設定。Primary path 必須
絕對且不含 NUL，符號名稱非空且無 NUL。載入／卸載可執行使用者程式碼，可能阻塞，
OSAL 不承諾可取消、無配置或無例外。close 失敗時保留 handle，供明確重試。

新增操作碼 OpenLibrary=46、FindSymbol=47、CloseLibrary=48；既有代碼不變。
Windows 保留 GetLastError；POSIX dlerror 是文字而非 errno，目前僅保留操作與通用錯誤分類，
不偽造 native 數字。詳細 POSIX loader 診斷是尚待設計的限制；本輪不改 Common Error 布局。


本輪 Windows DynamicLibrary 行為由 Module System 的 `module.plugin` 測試涵蓋，
Debug／Release 全套各 55/55 通過；包含實際載入／符號、move／close、中文／空白路徑。
POSIX backend 尚未實機驗證；結果與限制見 [Module System](../module_system/README.md)。

# OSAL Process

OSAL 提供同一份 C++23 程序操作介面。框架只使用
`<vinc/osal/process.hpp>` 並連結 `vinc::osal`；CMake 依目標 OS 選擇後端。
公開標頭不包含原生程序型別或平台條件編譯。

目前提供程序管理、[通用 OSAL Socket](SOCKET.md) 及 [通用 OSAL Pipe](PIPE.md)；既有 Module 尚未改為獨立
程序。IPC 訊息協定、共享記憶體 ABI、模組就緒握手、heartbeat、自動重啟與
核心崩潰後的子程序政策均不在此版本內。

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
  `spawn_with_pipe()` 是雙向便利組合。尚不提供 stdout／stderr capture。
- `OsError` 包含統一分類、失敗操作與原生 `std::error_code`；沒有原生錯誤
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
Result<Process> start_child(ProcessService& central,
                            const std::filesystem::path& absolute_executable) {
    SpawnOptions options;
    options.executable = absolute_executable;
    return central.spawn(options);
}

Result<ExitStatus> stop_child(ProcessService& central, ProcessId id) {
    auto child = central.find(id);
    if (!child) return std::unexpected(child.error());
    auto stopped = central.terminate(id);
    if (!stopped) return std::unexpected(stopped.error());
    return child->wait_until(std::chrono::steady_clock::now() + 5s);
}
```

中央透過 `ProcessService::create()` 建立並長期持有服務及 ID，處理退出
結果後呼叫 `forget(id)`。終止失敗或等待逾時後，仍可使用相同 ID 繼續處理。
全部程序處理完畢後，以 `shutdown_until()` 完成正常關閉流程。

## 建置與驗證

OSAL 可單獨建置，不依賴 message generator 或其他 VINC 模組：

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

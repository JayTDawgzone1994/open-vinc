# OSAL Pipe：通用單向位元組串流

OSAL Pipe 提供三平台共用的 `PipeReader`、`PipeWriter` 與非同步操作介面。
`create_pipe()` 不建立程序、不讀取啟動環境，也不認識 Module、Shell、CMD、
heartbeat 或 message bus。雙向通訊由兩條單向 Pipe 組合。

Windows 後端使用私有單向 named pipe 與 overlapped I/O／IOCP；Linux 使用
匿名 nonblocking pipe 與 epoll；macOS 使用匿名 nonblocking pipe 與 kqueue。
公開介面不暴露原生 handle／fd，不使用 socket，不新增第三方套件。
Windows 已實際驗證；Linux／macOS 已更新後端，但仍待目標平台建置與測試。

## 邊界與公開介面

| 標頭 | 責任 |
| --- | --- |
| `<vinc/osal/pipe.hpp>` | `Pipe`、`PipeReader`、`PipeWriter`、`PipeService`、buffer 與完成事件 |
| `<vinc/osal/process_pipe.hpp>` | 把指定端點交給子程序、取得繼承端點、雙向便利組合 |
| `<vinc/osal/error.hpp>` | 共用的 `Result`、`OsError`、`Deadline` |

消費者連結 `vinc::osal`；只使用 Pipe 時不必引入 Process 公開標頭。
基本關係是 `writer → reader`。`Pipe` 是兩個方向不同的擁有型端點，不是一個
可雙向讀寫的物件。讀寫端型別在編譯期限制可用操作，兩端各有自己的 `PipeId`。

- `PipeService::create_pipe()` 原子保留兩個端點名額，建立失敗時回收全部名額與資源。
- 端點可移動、不可複製；兩端可各自移動、關閉或交付。沒有公開複製原生端點的 API。
- 端點隸屬建立／接手它的 `PipeService`；用其他服務提交操作回報 `InvalidArgument`。
- 本版保留單一非同步模型，不另外提供同步收發、借用 buffer 的 `try_read/write()`
  或 `SocketPoller` 整合。Windows 使用完成通知，POSIX readiness 在後端轉成完成結果。
- 不提供訊息邊界、文字編碼、原生 handle 匯入、公開命名管道、監聽或連線探索。
  本版亦不提供任意子程序的 stdin／stdout／stderr 重導；OSAL 子程序需接手交付端點。

## 建立與收發

以下為操作順序示意；每個 `Result` 都必須檢查，完整錯誤處理與實際資料驗證
見 `tests/pipe_stream_tests.cpp`：

```cpp
#include <vinc/osal/pipe.hpp>
using namespace vinc::osal;

auto service = PipeService::create();
// 檢查 service。
auto pipe = service->create_pipe();
// 檢查 pipe；不需要 ProcessService。
auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
auto read = service->submit_read(pipe->reader, IoBuffer(4096), deadline);
// 檢查 read；成功後 OSAL 持有 buffer。
IoBuffer data(3);
data.bytes()[0] = std::byte{1};
data.bytes()[1] = std::byte{2};
data.bytes()[2] = std::byte{3};
auto write = service->submit_write(pipe->writer, std::move(data), deadline);
// 檢查 write，透過 wait_completions() 取得兩個操作各自的結果。
```

- `IoBuffer` 擁有自己的儲存空間。提交拒絕或提交前配置拋出例外時，不移走 buffer；
  成功接受後，舊指標／span 不再供呼叫端使用。
- 每個接受的操作產生一筆完成事件，歸還原 buffer。服務解構放棄結果時會安全丟棄。
- 每個端點同時最多一筆操作，未收集的完成事件也占用該端點及操作名額；再次提交為 `Busy`。
- 收發是 **some** 語意：成功時可能只傳輸部分資料。零長度 buffer 為 `InvalidArgument`。
  不保證一次 read 對應一次 write，也不承諾跨平台多寫入者的原子訊息語意。
- `IoCompletion` 回報 `IoId`、端點 `PipeId`、方向、status、已確認傳輸數量、
  可選原生錯誤與 buffer。只可讀取 `buffer.bytes().first(transferred)`。
- 成功寫入不代表對方已讀取或處理；確認與重送策略由上層協定負責。
- ID 在同一份已載入 OSAL 的執行期間不重用；跨程序識別使用另外的協定身分。

## EOF、取消與關閉

| 結果 | 意義 |
| --- | --- |
| `Completed` | 這次傳輸成功，可能只完成部分位元組 |
| `EndOfStream` | 讀取端觀察到寫入方向已結束 |
| `PeerClosed` | 寫入端觀察到讀取方向已關閉 |
| `Cancelled` | 操作已完成取消 |
| `TimedOut` | 操作尚未開始即過期，或到期發出的取消已完成 |
| `Closed` | 本端／服務關閉造成操作結束 |
| `Failed` | 其他原生錯誤，保留 `OsError` |

所有寫入端副本都關閉，且已送入的資料讀完後，讀取才得到 EOF。
程序退出與 EOF 不等價：程序可先關閉端點而繼續執行，其他程序的副本也可能
延後 EOF。正常結束寫入應先收集所有 write 完成結果，再關閉 writer，讓 reader
繼續讀到 EOF。關閉 reader 會使後續寫入觀察到斷管。

`close()` 可重複呼叫，只發起關閉；不保證排空未完成的操作，也不終止程序。
新提交回報 `Closed`，未完成操作會取消；原生 I/O 真正完成後才釋放端點與 buffer。
析構或 move assignment 取代舊端點也會關閉舊端點。端點的 bool 表示持有控制物件，
不表示端點仍開啟；移動後來源為空，明確 close 後原物件仍可查詢 id。

兩條 Pipe 組成雙向通道後，可只關閉其中的 writer，反方向仍繼續工作。
這是獨立端點的生命週期，不另造 Pipe 的 socket-style shutdown 模式。

`cancel(id)` 是要求取消，成功完成仍可能贏得競爭。已完成但未收集時為
`AlreadyCompleted`，已收集或未知 ID 為 `NotFound`。`Deadline` 使用 `steady_clock`；
到期觸發取消，不是清理完成時間的硬即時保證。取消、逾時或失敗不能證明對方收到
零個位元組，尤其 Windows 原生取消未必能回報全部已送出資料；OSAL 不會自動重送。

`wait_completions(output, deadline, stop_token)` 的取消／逾時只停止等待，
不取消已提交的操作。已就緒的結果優先，output 不得為空。服務停止且沒有剩餘
完成結果時為 `ServiceClosed`。

`shutdown_until()` 停止接受新工作、關閉所有端點，等待原生清理與工作執行緒結束。
成功後仍可取走完成結果。等待取消／逾時後清理繼續，buffer 不會提前釋放。
只解構服務會發起清理並放棄結果；背景狀態保留到原生 I/O 結束，不能以
use-after-free 換取表面的關閉成功。本版不承諾清理中的動態庫熱卸載。

## 通用程序端點交付

`spawn_with_pipes(processes, pipes, options, bindings)` 接受呼叫端已建立的端點。
每個 `PipeBinding` 是呼叫端自訂的 `uint32_t slot` 與 `PipeReader*` 或 `PipeWriter*`。
slot 不對應 OS fd，也不自動代表 stdin／stdout。一次可交付 1～64 個端點，
slot 與端點均不可重複；端點必須隸屬傳入的 `pipes`。

```cpp
// 檢查建立結果後，將一條 Pipe 的 reader 交給子程序；父程序使用 writer。
std::array<PipeBinding, 1> bindings{{{42, &pipe->reader}}};
auto child = spawn_with_pipes(processes, *service, options, bindings);
```

所有權與失敗契約：

1. 欲交付的端點必須尚未接受過 I/O。已使用或正在交付為 `Busy`，已關閉為 `Closed`。
   另一端可以已使用，甚至有待讀資料；限制只針對交付的端點。
2. 成功建立程序後，父程序交付端點會關閉並同步釋放其原生副本與名額；保留端點不受影響。
   成功不等於子程序已接手或就緒。
3. 啟動失敗／例外時，交付端點與資料維持原狀，臨時繼承副本全部回收；若呼叫端同時
   明確 close 或 shutdown，仍遵守該關閉要求。
4. 交付期間固定住原生資源並拒絕新 I/O。shutdown 會等待已進行的交付結束，不能把
   關閉 PipeService 當成撤銷正在建立的程序。呼叫端應先停止啟動工作，再關閉服務。
5. 子程序啟動時呼叫自由函式 `take_inherited_pipes(service)`，取得帶 slot 的
   `variant<PipeReader, PipeWriter>` 清單。這個動作不屬於 `PipeService` 的傳輸介面。
   接手流程只可 claim 一次；claim 後即使容量不足或配置失敗，也會關閉已取得的全部
   原生資源，不提供重試接手。應在啟動初始化時完成，避免同時修改程序環境。
6. 綁定端點、清單、服務物件的移動／解構不得與交付並行；存活物件的 close、
   提交與 shutdown 由內部同步。不同端點的程序啟動可並行。

Windows 端點第一次成功提交操作時才綁定 IOCP。已綁定的原生 handle 不能任意
改綁另一個完成埠，因此本版統一要求在使用前交付，明確拒絕已使用端點，避免
跨程序完成事件落到錯誤服務。這是交付限制，不限制普通收發與取消後繼續使用。
參考 [Microsoft CreateIoCompletionPort 契約](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-createiocompletionport)。

程序整合層使用保留變數 `VINC_OSAL_PIPE_BOOTSTRAP` 傳遞私有 v2 manifest。
普通 spawn 會移除這個變數；呼叫端不能覆寫。舊 v1 manifest 明確拒絕，不承諾
跨版本 OSAL 啟動協定／二進位相容。它不是任意原生 handle 匯入 API 或安全沙箱。

Windows 只建立臨時可繼承副本，並列入每次 spawn 的 handle 白名單；一般端點
預設不可繼承。POSIX 先將副本配置在 fd 67 以上，file actions 再映射到子程序
fd 3 起的連續位置，避免多端點重映射覆蓋來源；接手後設回 close-on-exec。
macOS 使用 pipe + fcntl 設定旗標，不能對宿主自行發動的並行 fork/exec 承諾
原子不繼承；OSAL 自己的 spawn 採明確白名單。Windows 亦不承諾外部自行使用
無白名單的全域 handle 繼承流程不會帶走臨時副本。

可以分別把同一條 Pipe 的兩端交給兩個子程序，讓它們直接通訊，父程序只負責交付。
傳輸格式與程序角色由呼叫端決定。

## 雙向便利組合與遷移

`spawn_with_pipe()` 現在透過兩次 `create_pipe()` 與 `spawn_with_pipes()` 組合：
父程序取得 `ProcessWithPipe{process, DuplexPipe{reader, writer}}`。
子程序使用 `take_inherited_pipe(service)`，約定 slot 0 為子程序 reader、slot 1 為 writer。
`DuplexPipe::close()` 關閉兩端；需要單方向結束時呼叫 `pipe.writer.close()`。
便利組合啟動時需要四個端點名額，成功交付後父程序留下兩個。
失敗路徑會同步回收未公開端點，避免連續失敗耗盡容量。

舊 API 的遷移：

- `PipeEndpoint` → 按用途改為 `PipeReader`、`PipeWriter`；雙向組合使用 `DuplexPipe`。
- `submit_read(child.pipe, ...)` → `submit_read(child.pipe.reader, ...)`。
- `submit_write(child.pipe, ...)` → `submit_write(child.pipe.writer, ...)`。
- `service.take_inherited_pipe()` → `take_inherited_pipe(service)`，引入 `process_pipe.hpp`。
- `max_endpoints` 現在以單向端點計數，不能沿用舊版每個雙向通道一個名額的估算。

## 並行、容量與成本

每個 `PipeService` 建立一條共用 I/O 工作執行緒，不為每條管道或每次提交開新執行緒，
也不在工作執行緒執行使用者 callback。不同端點可同時操作，同端點提交與 close
會序列化。多個完成等待者分取結果，不會各收到同一筆的副本。

預設上限為 256 個端點、512 筆操作（含未收集結果）、16 MiB OSAL 持有的 buffer
容量、單次傳輸 64 KiB。額滿立即回報 `ResourceExhausted`，不阻塞提交者等待名額。
預算依 buffer capacity 計算，不包含 OS 管道緩衝區、管理結構與呼叫端自身的 buffer。
一般 close 為非同步，端點名額在原生清理後回收。建立、接手或保留服務容量可拋出
標準 C++ 配置例外，不宣稱所有 API 為 noexcept。

事件迴圈掃描有界操作清單，尚無大量端點的效能量測。POSIX 只在 I/O 工作執行緒
封鎖 SIGPIPE，斷管寫入回報結果，不改寫宿主全域 signal handler。
`monitor_error()` 回報基礎設施錯誤，停止接受新工作並進行清理；若無法確認原生
操作已結束，仍保留其記憶體，shutdown 可逾時。

## 建置與驗證

```sh
cmake -S src/vinc/osal -B build/osal -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/osal
ctest --test-dir build/osal --output-on-failure
```

`osal.pipe_stream` 僅引入 Pipe 公開標頭，測試獨立串流、部分收發與完整性、讀完再 EOF、
單方向關閉、背壓、逾時／取消、端點容量的原子保留、不同服務拒絕、移動／關閉及資源回收。
`osal.pipe` 保留既有程序通訊測試，另測任意 slot、多端點與 64 端點白名單交付、
失敗保留、重複／已使用端點拒絕、兩個子程序直接通訊、未交付端點不洩漏、半關閉、交付與 shutdown 競爭，以及子程序接手失敗後的資源回收。
`osal.process`、`osal.socket` 同時回歸驗證。

2026-09-18：Windows 乾淨 Debug 與完整專案 Release 建置通過，兩種組態的上述
四個 CTest 套件全部通過；此次 OSAL 編譯未產生警告。完整專案原有 SHA／訊息
生成程式的數值轉型警告仍存在，未在本次修改。

Windows 工具鏈：x64 MSVC 19.44、SDK 10.0.26100、CMake 3.31.6、Ninja 1.12.1。
Linux／macOS 仍需實際建置與行為驗證，不以 Windows 測試代替三平台支援證據。

原生契約參考：

- [Windows overlapped pipe I/O](https://learn.microsoft.com/en-us/windows/win32/ipc/synchronous-and-overlapped-input-and-output)
- [取消 I/O 與儲存空間生命週期](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)
- [Windows named pipe 存取模式](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-createnamedpipew)
- [Linux pipe 串流與 EOF](https://man7.org/linux/man-pages/man7/pipe.7.html)

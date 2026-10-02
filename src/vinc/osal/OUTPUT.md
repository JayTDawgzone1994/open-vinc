# OSAL 子程序標準輸出擷取

`<vinc/osal/process_pipe.hpp>` 的 `spawn_with_output()` 將 Process 與 Pipe 組合，
擷取任意子程序的 stdout／stderr。子程序不需要連結 OSAL、使用 Vinc API 或讀取
啟動協定；一般 `printf`、`fprintf`、`std::cout`、`std::cerr` 即可使用。
OSAL 本身也不依賴 Module、Logger、Shell、Tcl 或 message bus。

## 公開介面與模式

```cpp
Result<ProcessWithOutput> spawn_with_output(
    ProcessService&, PipeService&, const SpawnOptions&,
    std::span<const PipeBinding> bindings = {});

struct ProcessWithOutput {
    Process process;
    std::optional<PipeReader> stdout_reader;
    std::optional<PipeReader> stderr_reader;
};
```

`SpawnOptions::stdout_mode` 與 `stderr_mode` 各自設定，不合併串流：

| 模式 | 行為 | 回傳 reader |
| --- | --- | --- |
| `Inherit` | 繼承父程序對應輸出；缺少標準串流時接空裝置 | 無 |
| `Null` | 接空裝置 | 無 |
| `Capture` | 接獨立的 Pipe，子程序寫入端採阻塞模式 | 有 |

原有預設不變：stdin 為 `Null`，stdout／stderr 為 `Inherit`。
stdin 的 `Capture`、無效 enum 值，以及在 `ProcessService::spawn()`、
`spawn_with_pipes()` 或 `spawn_with_pipe()` 使用 `Capture`，均回報 `InvalidArgument`。
需要擷取並交付通用 Pipe 時，使用 `spawn_with_output()` 的 bindings 參數。
最多交付 64 個通用端點；兩條標準輸出不占用 binding slot。

## 讀取順序

以下是設定與提交順序示意；每個 Result 都需要檢查。完整、有錯誤清理與逾時的
雙路讀取範例見 `tests/process_output_tests.cpp` 的 Harness 與 `drain()`。

```cpp
SpawnOptions options;
options.executable = absolute_executable_path;
options.stdout_mode = StdioMode::Capture;
options.stderr_mode = StdioMode::Capture;
auto child = spawn_with_output(processes, pipes, options);
// Check child before accessing its readers.
auto out = pipes.submit_read(*child->stdout_reader, IoBuffer(4096), deadline);
auto err = pipes.submit_read(*child->stderr_reader, IoBuffer(4096), deadline);
// Check both submissions. Collect completions through pipes.wait_completions().
// Identify each stream by PipeId; consume exactly `transferred` bytes and
// resubmit that reader until EndOfStream. Keep both streams draining together.
```

資料是原始 bytes，可含 NUL；OSAL 不指定編碼、不切行、不增加日誌標頭。
一次讀取可能只得到部分資料，UTF-8 字元也可能跨越多次完成事件。
各串流保留自身的位元組順序；兩條串流之間不承諾總排序。
子程序的 CRT／iostream 緩衝與文字模式轉換由子程序決定，OSAL 不強制 flush。

**Pipe 滿了會阻塞子程序寫入。** 呼叫端應同時持續讀取兩路，不要先等程序退出才
開始讀取，也不要讀完 stdout 才讀 stderr。這兩種做法都可能與子程序互相等待。
OSAL 不暗中丟資料或增加無界暫存；日誌排隊、慢消費者與轉送政策由上層決定。

## 所有權、失敗與關閉

- 回傳值可移動、不可複製。每條 Capture 使用一個 `PipeService::max_endpoints`
  名額；子程序端不登記為公開 PipeWriter。I/O 仍受既有操作數量與 buffer 上限限制。
- 建立程序前先完成資源與容量準備。失敗時回收這次建立的 Pipe、繼承副本及名額；
  同次交付的通用 bindings 保持原來的所有權。成功後只做不需配置的所有權發布。
- 成功回傳前，父程序關閉自己持有的所有子程序輸出 writer 副本。公開 reader 不可
  被子程序繼承。僅擷取標準輸出時，不建立 `VINC_OSAL_PIPE_BOOTSTRAP` 環境變數。
- `Process` 與 reader 生命週期獨立。退出後仍能讀到已進入 Pipe 的尾端資料；
  子程序提前關閉輸出時，也能在程序仍存活時收到 EOF。
- EOF 要等所有 writer 副本關閉，且緩衝讀完。如果子程序讓孫程序繼承標準輸出，
  EOF 可能晚於直接子程序退出。讀取期限與收尾等待政策由呼叫端決定。
- read 的取消／逾時只影響該 I/O，不關閉 reader、不終止程序；可再次提交讀取。
- 關閉 reader 或 PipeService 會取消未完成讀取，且不保證排空資料。
  OSAL 不因此要求 OS 終止子程序，但子程序後續寫入可能失敗；POSIX 的預設
  `SIGPIPE` 動作可能終止寫入者。因此「停止顯示」應在上層停止轉送，並繼續讀取。
- 先停止新的啟動工作，再關閉服務。與 shutdown 競爭時，已接受的啟動仍可能成功，
  但回傳 reader 可能已受關閉要求影響。服務、options、bindings 及端點物件的
  移動／解構不得與啟動並行；一般 close／shutdown 沿用內部同步。
- 擷取不會接管父程序本身的 stdout／stderr，也不提供終端模擬或 PTY。

## 平台實作

- Windows：私有 byte-mode named pipe；父端 overlapped／IOCP，子端同步寫入。
  `STARTF_USESTDHANDLES` 與每次啟動的明確 Handle 清單交付標準輸出。
- Linux／macOS：匿名 Pipe；父端 nonblocking，新的子端 writer 清除 `O_NONBLOCK`。
  啟動時先透過 `dup2` file actions 設定 fd 1／2，再關閉未交付的 fd。
  臨時來源 fd 避開通用端點的映射範圍，兩種用途可同時使用。

公開 API 不暴露原生 Handle／fd；使用既有 PipeService worker，沒有為每個子程序
新增讀取執行緒或第三方相依套件。外部程式自行使用非白名單繼承的限制，沿用
[Pipe 端點交付說明](PIPE.md)。

## 驗證與獨立建置

OSAL 可直接作為 CMake 根專案建置，例如在已配置的 C++23 工具鏈環境：

```text
cmake -S src/vinc/osal -B build/osal-standalone -DBUILD_TESTING=ON
cmake --build build/osal-standalone --config Release
ctest --test-dir build/osal-standalone -C Release --output-on-failure
```

新增的 `osal.process_output` 涵蓋一般程式的 C／C++ 輸出、雙路大型二進位資料、
獨立模式組合、容量拒絕、啟動失敗回收、移動、逾時／取消／關閉、提前 EOF、
強制終止、崩潰前已 flush 資料，以及與通用 Pipe 交付的組合。
Windows 已執行驗證；Linux／macOS 後端已實作，但仍待各平台實機建置及行為驗證。

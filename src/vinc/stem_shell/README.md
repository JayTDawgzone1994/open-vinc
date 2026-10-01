# 互動式 Shell

Shell 是中央的操作介面，模組命令仍經 `ModuleManager → control Socket → BaseModule virtual hook`。
訊息 Bus payload 不經 Shell 或 Socket 傳遞。

## 分工與所有權

- `main.cpp`：初始化 Tcl、建立 ModuleManager／Shell、先保留命令名稱再登記 CMake 模組目錄，處理 `-S`。
- `CommandRouter`：共用命令派送、完整錯誤與補全結果；Shell 管理 `help`／`exit`／`srun`／`scancel`。
- `TclRuntime`：每次腳本建立獨立 Tcl interpreter，工作執行緒持有、執行及銷毀；支援取消與有界輸出暫存。
- `command_line`：引號與參數解析、128 筆歷史、4096-byte 輸入上限與 UTF-8 行尾編輯。
- `StemShell`：終端事件迴圈、命令工作與補全結果顯示、完整關閉流程。
- OSAL `Terminal`：Windows console／POSIX terminal 的模式管理、按鍵、UTF-8、EOF、SIGINT／Ctrl+C。
  終端與停止事件處理在啟動腳本之前啟用，離開時還原。

`StemShell(manager, output)` 明確借用 manager 與輸出；它們必須比 Shell 活得久。
嵌入 Shell 的程式須先呼叫 `vinc::shell::initialize_tcl(argv[0])`，執行檔旁必須有 `lib/tcl9.1`。
預設建構子保留 singleton 相容入口。Shell 的 UI／process_line／生命週期操作由同一控制執行緒使用，
不可與另一個 `run`／析構並行。模組工作在框架內部的工作執行緒進行。
全程序同時只允許一個 OSAL Terminal 擁有標準輸入，第二個 open 回 Busy。

ModuleManager 保留框架命令及初始化後的 Tcl 全域命令／autoload 名稱，直到 manager 析構；不持有指向 Shell 的 callback。
factory 與 `node_base_name` 使用保留字時直接拒絕，不靠 `_1` 避開；既有名稱衝突會在 Shell 初始化時回報。
先建構 Shell 再登記模組可讓錯誤發生在登記當下。腳本自己的 `proc` 等名稱仍應避免遮蔽模組命令。
重新建立 Shell 不會呼叫已析構 Shell 的函式；factory／實例名稱不會與這些內建命令衝突。
既有 `register_micro_program` 仍可用，其 callback 是中央原生程式碼，需自行保證會返回；
本次未將 legacy micro-program 改成獨立程序。

## 命令與參數

```text
help
exampleModule
exampleNode hello
busExample sub demo
busExample pub demo
busNode_1 push 42
busNode last
exit
```

互動式送出模組命令後會立即回到輸入。模組啟動完成會印出 `Started <完整節點名>`；
在 Ready 前操作該節點會得到未就緒錯誤。`help` 顯示已登記種類與所有實例的狀態、PID、最後錯誤，
不把所有非終止狀態都標成 Running。

引號保留空白與空參數，雙引號支援 `\"`、`\\`；單引號內保留所有文字。
引號外可用反斜線跳脫空白、引號或反斜線；其他反斜線保留，方便 Windows 路徑。
引號可以與同一參數的未引用片段相接。未關閉引號拒絕執行。

```text
node open "D:\Some Folder\a.txt"
node set label ""
node set path '\\server\share\file'
```

互動解析不把 `#`、`//` 視為註解，也不解釋 `$`，故 URL 等參數原樣保留。
單行上限 4096 bytes、256 個參數；超長行拒絕整行，不執行被截斷的前半段。

### 本次保留的 hook 相容性

尚未變更 hook 的既有位置約定：Shell 送入 `on_start` 的參數包含 factory 名，
送入 `node_interface`／`node_autoComplete` 的參數包含完整 node 名。
直接呼叫 ModuleManager 的 API 則原樣傳遞呼叫端提供的 vector，不擅自增刪。
Bus 範例仍兼容兩種呼叫來源。若未來統一移除目標名稱，需同步遷移各 hook。

## 命令執行與錯誤

- 互動介面最多 16 個執行中的命令／補全工作；滿載立即回 Busy，沒有未執行命令的無界佇列。
- 同一目標仍有 Shell 工作時拒絕重疊送出；不同目標可以並行。
  ModuleManager 另外確保同一實例只能有一個未完成的遠端請求。
- 節點命令等待 5 秒，補全等待 1 秒，啟動等待該模組 startup_timeout 加一秒。
  等待在工作執行緒，鍵盤、內建 help 與退出仍可處理。
- Timeout 表示命令結果未知，不代表未執行或已取消；不自動重送。
  ModuleManager 會保留該請求的 Busy 狀態直到遲到回覆或該次程序退出，避免繼續往卡住的模組排隊。
- 顯示完整的 Timeout／Closed／Busy／Protocol／IO 與原生 OS 資訊，
  遠端 hook 非零回傳則顯示實際回傳碼。未知命令、解析錯誤會直接印出。
- Tab 同樣非同步；結果抵達時，若輸入已改變便丟棄舊結果。
  多候選全部顯示，不要求候選本身是全域節點；單候選會依引號規則填入。

不同程序仍直接繼承 stdout／stderr；非同步日誌可能與 prompt 交錯。
本次未加入集中日誌擷取或命令文字回覆協定，命令結果本身仍是狀態碼。

## Ctrl+C／exit 的完整關閉流程

第一次 Ctrl+C 或 `exit` 啟動同一流程，先取消執行中的腳本；重複 Ctrl+C 不跳過清理，也不直接 `_Exit`。
一般 Shell 的輸入 EOF 也會關閉；以 `-S` 啟動且 stdin 非互動終端時，EOF 只表示沒有本機輸入，框架繼續運作。

模組透過 OSAL 建立獨立程序群組，讓終端的 Ctrl+C 由中央接收並協調停止，
避免子程序先被 OS 預設處理器結束而跳過 `on_stop()`。Windows 使用 `CREATE_NEW_PROCESS_GROUP`，
POSIX 使用 `POSIX_SPAWN_SETPGROUP`；沒有建立新 console，也沒有加入整棵程序樹的終止政策。

1. 關閉中央的新註冊／啟動入口，Shell 停止接收新命令。
2. 同時標記所有受管理實例停止，停用其自動重啟。
3. 經既有控制通道通知模組，讓 `on_stop()` 與析構正常完成。
4. 超過每個模組的 stop_timeout，監督執行緒透過 OSAL 要求 OS 終止。
5. 持續觀察 OS 的退出結果；只提出 terminate 請求不算完成。
6. 等待 Bus Broker 已排入的退出回收工作完成，收完 Shell 工作，才顯示關閉完成並還原終端。

`ModuleManager::begin_shutdown()` 為可重複呼叫的停止入口；`wait_shutdown(deadline)` 的 Timeout
不會停止監督。Shell 每次短暫等待後顯示尚未退出的名稱、PID 與終止錯誤，持續等到真正完成。
OS 拒絕終止或程序無法被排程時可能持續等待，沒有「第二次 Ctrl+C 直接丟下子程序」的捷徑。
範圍是 ModuleManager 持有的程序，不包含使用者自行建立且未交給框架管理的程序。

一般 C++ 呼叫端應明確使用上述兩階段 API；ModuleManager 原有析構仍是最後的防護路徑。
Shell 成功返回關閉流程時，所有受管理程序都已確認退出，無須以析構的時間限制猜測成功。

## 終端行為與平台邊界

- 上／下取回歷史，返回最新位置時還原尚未送出的草稿；不記錄連續重複命令。
- 支援行尾輸入與 Backspace；刪除一個完整 UTF-8 code point，避免留下半個中文字元。
  本次未實作中間插入／游標左右移動、完整 grapheme cluster（組合字／複合 emoji）編輯。
- Windows 使用 ReadConsoleInputW 並處理 UTF-16 代理對；POSIX 解碼 UTF-8 與方向鍵 escape sequence。
  單獨 Escape 有短暫辨識期限，不會無限等後續按鍵。
- 顯示長行末端，以非 ASCII 每字最多兩欄保守預留；不宣稱精確覆蓋所有終端的字形寬度。
- 重導向的 UTF-8 stdin 區分 EOF 與錯誤，不會空轉；stdout 非終端時不輸出 ANSI prompt。
  這個入口沿用互動的非同步派送，不能將逐行輸入當成同步腳本執行保證。
- Ctrl+C 按鍵與程序 SIGINT／Windows CTRL_C_EVENT 只傳達停止事件；清理在一般執行緒進行。
  OS 強制關機／終端強制關閉另有 OS 時限，不承諾被 OS 強殺後仍能完成清理。

Windows console 模式依據 [Microsoft GetConsoleMode](https://learn.microsoft.com/en-us/windows/console/getconsolemode)，
POSIX 模式及等待依據 [termios](https://www.man7.org/linux/man-pages/man3/termios.3.html) 與
[poll](https://www.man7.org/linux/man-pages/man2/poll.2.html)。

## Tcl 腳本

舊 `ScriptInterpreter`、自訂語法、`run_script` API 與 `-sc` 已移除。使用標準 Tcl 9.1，慣用副檔名 `.tcl`，檔案採 UTF-8。
互動 Shell 仍使用上面的命令列語法，不將一般輸入當作 Tcl 程式執行。

從專案根目錄啟動：

```powershell
.\build\windows-msvc-debug\Vinc.exe -S examples/scripts/startup.tcl
```

Shell 中：

```text
srun examples/scripts/maintenance.tcl
srun examples/scripts/cancel-demo.tcl
```

`Vinc -S file.tcl [args...]` 與 `srun file.tcl [args...]` 共用執行器。
參數透過 Tcl `argv`／`argc` 提供，`argv0` 為腳本路徑。
每次執行使用獨立變數、proc 與事件環境；一次只執行一份腳本。
`source` 可在同一次環境引入檔案，嵌套 `srun` 則回 Busy。
路徑相對於程序目前工作目錄，不自動 `cd`。相對於腳本所在目錄引入檔案可寫：

```tcl
source [file join [file dirname [info script]] common.tcl]
```

模組命令直接依名稱派送，Tcl 的變數／清單／引號先解析成參數陣列，不再拼成字串重做 Shell 解析：

```tcl
set action hello
exampleModule
exampleNode $action
if {[catch {exampleNode status} reason]} {
    puts "Command failed: $reason"
}
```

- 命令依序等待完成；啟動等 Ready 後才執行下一行。成功回傳 `0`，啟動時另外顯示完整名稱。
- 自動尾碼保留，腳本作者負責呼叫正確的完整實例名稱；框架不改寫腳本中的目標。
- 非零 hook 回傳、未知命令、啟動失敗、逾時轉為 Tcl error，可由 `catch` 處理。
- 未處理錯誤顯示檔名、行號及 Tcl 呼叫堆疊；包括 `-S` 在內，都只終止該次腳本，框架與已啟動模組繼續運作。
- 本機 Shell 前景等待腳本時，忽略一般輸入，保留 Ctrl+D／Ctrl+C；完成後恢復 prompt。
  先前已送出的非同步命令仍可能執行，因此腳本不是交易或全域互斥區段。Socket Shell 尚未實作。
- Ctrl+D 與 `scancel` 都要求取消腳本，沒有腳本時只提示；Ctrl+D 與 stdin EOF 是不同事件。
  本機前景執行期間使用快捷鍵；腳本也可用 `scancel` 取消自己。
- `return` 正常結束目前腳本檔案。Tcl 的 `exit` 改接框架完整關閉，只接受無參數形式，不能直接跳過清理。
- 取消採 Tcl `TCL_CANCEL_UNWIND`，一般 `catch` 不能攔截；純 Tcl 迴圈、`after ms` 與框架提供的模組命令／啟動等待可響應取消。
  已送出的命令不撤銷，遲到回覆／程序退出前仍保持 Busy；已完成操作不回滾，已啟動模組不自動停止。
- 原生擴充、原生 micro-program、阻塞檔案／外部程序操作仍可能延遲取消；不強殺中央執行緒。
  腳本應將長時間工作交給模組，且不從 stdin 與 Shell 搶讀輸入。
- Tcl 標準功能保留，腳本是受信任的程序內部署程式，不是隔離的安全沙箱。
  定時 callback 只在該次 Tcl 環境存在期間有效，不會在腳本結束後繼續運作。
- Tcl `puts` 的 stdout／stderr 經 Shell 輸出，每次尚未消費資料上限 256 KiB，超量標記截斷；寫檔仍使用 Tcl 原有行為。

### 建置與部署

Tcl 使用未修改的固定上游，透過 [library/tcl](../../../library/tcl/README.md) 整合。
使用單一組態的 Ninja 建置，Debug／Release 分開目錄；Windows 使用 MSVC／NMAKE，Linux／macOS 路徑使用上游 configure／make 與 zlib 開發套件。
Tcl 核心靜態連結，初始化腳本、編碼資料及授權放在 `Vinc` 旁的 `lib/tcl9.1`，不要求安裝系統 Tcl。
部署時須一併保留該目錄。現有自動生成的模組目錄仍記錄建置時的模組絕對路徑；Tcl 資源可搬移不代表整套模組部署已支援任意搬移。

## 驗證

- `shell.unit`：引號／空字串／跳脫、Windows 路徑、補全 roundtrip、中文刪除、歷史草稿、
  Shell 重建的 callback 生命週期、命名保留、原生回傳碼與候選清單。
- `shell.terminal`：重導向 UTF-8 分段、CRLF、方向鍵、孤立 Escape、Ctrl+C、非法 UTF-8、EOF、獨佔與釋放。
- `shell.integration`：真實 Module 程序的參數與補全，命令逾時後 Busy／遲到回覆，
  卡住命令期間仍操作其他模組；Ctrl+C 等待正常清理並強制終止卡住程序，確認退出；exit／EOF 共用流程。
- Windows 的 `shell.console`：在獨立隱藏 console 注入原生鍵盤事件，驗證中文、代理對、Ctrl+C、
  方向鍵、Backspace，以及 input/output mode 與 code page 還原；另外實際廣播 OS CTRL_C_EVENT，
  確認中央收到事件，模組經框架完成 `on_stop()` 並以正常退出碼結束。不是人工實機操作紀錄。
- `shell.tcl`：標準 Tcl 控制流程、source、argv、中文／空白／空參數、錯誤位置、獨立環境、保留字、
  catch 內無限迴圈／after／模組等待取消、取消後再次執行、前景輸入、啟動失敗保留框架、啟動期間 Ctrl+C 與真實模組退出。
- `shell.console` 另驗證原生 Ctrl+D、搬移後的 Tcl 資源、Unicode `-S` 路徑與參數、無 stdin 的失敗部署保持存活及 OS Ctrl+C 關閉。

本次在 Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2
完成建置與測試：Debug 20/20、Release 20/20 全數通過。
Linux／macOS 後端尚未進行原生建置與行為驗證。

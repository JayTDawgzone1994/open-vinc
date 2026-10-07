# 互動式 Shell

Shell 是中央的操作介面，模組命令仍經 `ModuleManager → control Socket → BaseModule virtual hook`。
訊息 Message Bus payload 不經 Shell 或 Socket 傳遞。

Vinc 預設開啟 `127.0.0.1:5500` 的 [Socket Shell](SOCKET.md)，可用 PuTTY Raw 連入；
啟動參數、Session 恢復與容量限制見該文件。

## 元件與中央程式

Shell 的框架接線標頭為 `<vinc/shell/detail/shell.hpp>`／`<vinc/shell/detail/socket_shell.hpp>`，target 為 `vinc::shell`。
SDK 只包含 `<vinc/shell/socket_shell_options.hpp>` 的設定資料與錯誤 enum，不公開服務啟停介面。
命令解析、router、script service 與 Tcl 接線標頭在 `include/vinc/shell/detail/`。
中央 main 與 Module catalog 的組裝已移到 `apps/vinc`；本元件維護可重用邏輯。

元件目錄與主檔名統一為 `shell`，框架內部類別為 `vinc::Shell`。
輔助型別的 `vinc::shell` 命名空間、`vinc_shell`／`vinc::shell` library target 保持原名。
中央程式 target 為 `shell`，輸出檔名仍為 `Vinc.exe`／`Vinc`；不提供舊名稱相容別名。

### 本機命令流程閱讀入口

- `src/command_line.cpp`：先檢查輸入大小，再解析引號／跳脫，最後檢查參數數量；
  `LineEditor` 獨立處理歷史草稿與 UTF-8 行尾。
- `src/command_router.cpp`：`execute()` 依序選擇 builtin、legacy command、Module、ModuleType；
  help 組裝、各類命令執行、補全候選收集分別由具名 helper 處理。補全保留自己的原有查找順序。
- `src/shell.cpp`：`submit()` 區分本機 Session 命令與非同步工作；`collect()` 依序收取
  Module 輸出、腳本結果、關閉要求與命令／補全結果；`handle_key()` 負責終端事件。
  `wait_shutdown()` 持續收取結果並等待程序退出，沒有新增整體關閉逾時。
- Shell 建構的輸出訂閱、Tcl 命令保留、工作儲存配置分成具名階段；訂閱由成員持有並於
  建構失敗時回收，名稱保留仍由 ModuleManager 持有，直到 manager 析構。

本機輸入／參數／歷史／工作數上限、輸出批次、命令／補全期限與輪詢間隔集中於
`<vinc/config.hpp>`；`vinc::shell` 直接公開相依 `vinc::common`。
數值沿用原實作；Session 身分與終端顯示常數留在 Shell。
SocketShell 與腳本設定也集中於同一標頭，沿用原值；詳細流程如下。

### 遠端與腳本流程閱讀入口

- `src/socket_shell.cpp`：建構依序驗證設定、解析位址、開啟 listener、登記 poller、
  保留 Session 儲存，最後啟動 worker；資源由成員持有並於建構失敗時解構。
  `loop()` 驅動 `advance_session()` 與 `handle_ready_event()`；前者收取結果、
  更新收送關注及斷線期限，後者依序處理讀取、傳送及連線錯誤。
  `handle_session_command()` 處理恢復、斷線、腳本狀態與輸出控制；一般命令仍非同步派送。
- `collect()` 固定依序收取命令結果、腳本結果、模組輸出，最後發出關機完成訊息。
  各 Session 的狀態由網路執行緒維護；跨執行緒關機要求使用原有 atomic。
  回覆佇列仍計入正在部分傳送的完整項目，不因已送出部分內容而提早釋放預算。
- `src/script_service.cpp`：控制操作持有 `mutex_` 收取已完成工作、處理 owner 信箱；
  `take()` 消費已收取內容，但執行中的 owner 保留信箱。工作結束後再次收取尾端輸出。
- `src/tcl_runtime.cpp`：`run()` 保持 interpreter 與 guard 的建立／銷毀順序；
  `install_commands()`、`install_arguments()` 負責環境設定，`handle_after()`、
  `handle_puts()`、`dispatch_command()` 區分原生命令橋接責任。
  interpreter 的公開指標仍在同一 mutex 保護下借出／解除，先解除再銷毀，最後清理 Tcl thread。
- `tests/`：Socket、Tcl、整合測試依情境拆成具名函式；Console、Terminal、Module helper
  與 InputPipe 展開控制流程和清理物件，保留既有測試順序、工作負載、斷言與失敗政策。

設定中的連線上限、poller 容量、保留 Session 數與腳本信箱數有相依關係：
預設 8 條連線，poller 額外登記 1 個 listener；一般 Session 最多保留連線數的 2 倍，
另留 1 個僅供恢復的入口。腳本信箱最多 17 個，涵蓋保留的遠端 owner 與本機 owner。
這些是既有容量政策的具名化；沒有增加執行時 Param 功能。


## 分工與所有權

- `apps/vinc/src/main.cpp`：初始化 Tcl、建立 ModuleManager／Shell、先保留命令名稱再登記 CMake 模組目錄，處理 `-S`。
- `CommandRouter`：共用命令派送、完整錯誤與補全結果；Shell 管理 `help`／`exit`／`srun`／`scancel`。
- `TclRuntime`：每次腳本建立獨立 Tcl interpreter，工作執行緒持有、執行及銷毀；支援取消與有界輸出暫存。
- `ScriptService`：本機與遠端共用一個 Tcl 工作；以 Session ID 路由輸出、結果與斷線取消。
- `SocketShell`：OSAL TCP／Poller 收送、整行輸入、Session 恢復、有界傳送與輸出訂閱。
- `command_line`：引號與參數解析、128 筆歷史、4096-byte 輸入上限與 UTF-8 行尾編輯。
- `Shell`：終端事件迴圈、命令工作與補全結果顯示、完整關閉流程。
- OSAL `Terminal`：Windows console／POSIX terminal 的模式管理、按鍵、UTF-8、EOF、SIGINT／Ctrl+C。
  終端與停止事件處理在啟動腳本之前啟用，離開時還原。

中央框架啟動固定執行：初始化 Tcl → 建立 ModuleManager → 建立 Shell 並保留命令名稱
→ 登記 Module catalog → 開啟 Socket Shell → 執行啟動腳本／操作迴圈。
腳本與 Socket Shell 是預設功能；使用者透過 CLI 調整 IP、port、timeout，不負責呼叫
`initialize_tcl()`、`Shell::create()` 或 `start_socket_shell()`。不新增服務啟用開關。
SDK include 本身不啟動中央服務；Module 子程序也不建立中央 Shell。

上述函式保留作框架內部接線，標頭位於 detail，不提供舊公開標頭的相容轉接。
`Shell::create(manager, output)` 回傳 Result owner，明確借用 manager 與輸出；兩者必須
比 Shell 活得久。中央先初始化 Tcl，缺少資源可由內部 Result 回報；沒有 singleton。
Shell 的 UI／process_line／生命週期操作由同一控制執行緒使用，不可與另一個 run／析構並行。
模組工作仍在框架內部工作執行緒進行。
全程序同時只允許一個 OSAL Terminal 擁有標準輸入，第二個 open 回 Busy。

ModuleManager 保留框架命令及初始化後的 Tcl 全域命令／autoload 名稱，直到 manager 析構；不持有指向 Shell 的 callback。
ModuleType 與 `module_base_name` 使用保留字時直接拒絕，不靠 `_1` 避開；既有名稱衝突會在 Shell 初始化時回報。
先建構 Shell 再登記模組可讓錯誤發生在登記當下。腳本自己的 `proc` 等名稱仍應避免遮蔽模組命令。
重新建立 Shell 不會呼叫已析構 Shell 的函式；ModuleType／實例名稱不會與這些內建命令衝突。
既有 `register_micro_program` 仍可用，其 callback 是中央原生程式碼，需自行保證會返回；
本次未將 legacy micro-program 改成獨立程序。

## 命令與參數

```text
help
exampleModule
exampleNode hello
messageBusExample --name subscriber sub demo
messageBusExample --name publisher pub demo
publisher push 42
subscriber last
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

尚未變更 hook 的既有位置約定：Shell 送入 `on_start` 的參數包含 ModuleType 名，
送入 `module_interface`／`module_autoComplete` 的參數包含完整 Module 名。
直接呼叫 ModuleManager 的 API 則原樣傳遞呼叫端提供的 vector，不擅自增刪。
Message Bus 範例仍兼容兩種呼叫來源。若未來統一移除目標名稱，需同步遷移各 hook。

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

模組 stdout／stderr 由 [OutputHub](../output_hub/README.md) 擷取，Shell 在事件迴圈中
批次顯示，再還原輸入列。模組輸出由 OutputHub TextRenderer 清除終端控制序列後呈現，不直接執行 ANSI 序列。
Shell 佇列有容量上限，過慢時略過最舊輸出並提示；命令結果與 Tcl 輸出仍走原本路由。
模組的任意 stdout 不會被當作某個請求的專屬回覆；命令 hook 的結果仍是狀態碼。
Shell 建構時即訂閱，因此應先建立 Shell，再啟動需要顯示初始化輸出的模組。

## Ctrl+C／exit 的完整關閉流程

第一次 Ctrl+C 或 `exit` 啟動同一流程，先取消執行中的腳本；重複 Ctrl+C 不跳過清理，也不直接 `_Exit`。
中央預設啟用 Socket Shell，stdin 非互動終端的 EOF 只表示沒有本機輸入，框架繼續運作。
框架內部測試若單獨建立未啟用網路／腳本的 Shell，輸入 EOF 仍走完整關閉流程。

模組透過 OSAL 建立獨立程序群組，讓終端的 Ctrl+C 由中央接收並協調停止，
避免子程序先被 OS 預設處理器結束而跳過 `on_stop()`。Windows 使用 `CREATE_NEW_PROCESS_GROUP`，
POSIX 使用 `POSIX_SPAWN_SETPGROUP`；沒有建立新 console，也沒有加入整棵程序樹的終止政策。

1. 關閉中央的新註冊／啟動入口，Shell 停止接收新命令。
2. 同時標記所有受管理實例停止，停用其自動重啟。
3. 經既有控制通道通知模組，讓 `on_stop()` 與析構正常完成。
4. 超過每個模組的 stop_timeout，監督執行緒透過 OSAL 要求 OS 終止。
5. 持續觀察 OS 的退出結果；只提出 terminate 請求不算完成。
6. 等待 Message Bus BusManager 已排入的退出回收工作完成，收完 Shell 工作，才顯示關閉完成並還原終端。

`ModuleManager::begin_shutdown()` 為可重複呼叫的停止入口；`wait_shutdown(deadline)` 的 Timeout
不會停止監督。Shell 每次短暫等待後顯示尚未退出的名稱、PID 與終止錯誤，持續等到真正完成。
OS 拒絕終止或程序無法被排程時可能持續等待，沒有「第二次 Ctrl+C 直接丟下子程序」的捷徑。
範圍是 ModuleManager 持有的程序，不包含使用者自行建立且未交給框架管理的程序。

框架內部接線應明確使用上述兩階段 API；ModuleManager 原有析構仍是最後的防護路徑。
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
- 不再自動添加尾碼；同名實例直接報錯。模組可在 `on_name(args)` 解析自己的命名選項，腳本呼叫核准後的完整名稱；框架不改寫腳本中的目標。
- 非零 hook 回傳、未知命令、啟動失敗、逾時轉為 Tcl error，可由 `catch` 處理。
- 未處理錯誤顯示檔名、行號及 Tcl 呼叫堆疊；包括 `-S` 在內，都只終止該次腳本，框架與已啟動模組繼續運作。
- 本機 Shell 前景等待腳本時，忽略一般輸入，保留 Ctrl+D／Ctrl+C；完成後恢復 prompt。
  先前已送出的非同步命令仍可能執行，因此腳本不是交易或全域互斥區段。其他 Socket Shell 可繼續操作；
  遠端腳本不占住本機輸入，本機仍可操作模組或用 `scancel` 取消全域腳本。
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
框架建置的 `vinc_deploy_tcl(target)` 預設跟隨 target 的實際輸出目錄，包含父專案後設的
`RUNTIME_OUTPUT_DIRECTORY`；多個 target 寫入同一目的地時在建置期間以檔案鎖序列化複製。
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

初版歷史紀錄：當時在 Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2
完成建置與測試：Debug 20/20、Release 20/20 全數通過。
Linux／macOS 後端尚未進行原生建置與行為驗證。

### Shell 命名統一驗證（2026-10-04）

本輪只統一目錄、主檔名、類別與中央程式 target；9 個受影響 C++ 檔案比對確認
僅包含名稱／路徑替換，未進行程式風格重構或改變執行行為。

- Windows x64 MSVC Debug：完整建置與 SDK 標頭編譯成功；Shell、SDK、
  Module／Message Bus 中央程式測試共 9/9 通過（17.20 秒）。
- Windows x64 MSVC Release：相關獨立測試目標與 SDK 標頭編譯成功；
  Shell Socket／Tcl／Unit／Terminal／Integration、SDK 測試 6/6 通過（12.70 秒）。
- Linux／macOS 本輪未驗證。

### 本機 Shell 可讀性整理驗證（2026-10-04）

本輪整理命令解析／編輯、CommandRouter、本機 Shell 主迴圈與單元測試，保留既有
派送順序、工作／腳本生命週期、例外邊界、診斷訊息及關閉等待政策。
補充解析與編輯的邊界案例：相鄰引號、不同引號狀態的跳脫、escaped tab、控制字元拒絕、
輸入／參數數量上限、補全插入位置，以及拒絕超長編輯後原內容不變。

- Windows x64 MSVC Debug／Release：完整建置與 SDK 個別標頭編譯成功。
- Shell Socket／Tcl／Unit／Terminal／Integration／Console、SDK、Module／Message Bus
  中央程式測試：Debug 9/9 通過（17.51 秒），Release 9/9 通過（16.85 秒）。
- 上輪占用 Release 執行檔的程序已結束；本輪已完成 Release 中央程式重建，
  並補驗 `shell.console`、`module.shell`、`message_bus.shell`。
- Linux／macOS 本輪未驗證；本次未重跑所有其他元件測試。

### 完整 Shell 可讀性整理驗證（2026-10-04）

本輪補齊 SocketShell、ScriptService、TclRuntime 的實作與標頭，以及七個尚未整理的
測試／輔助檔案。命令優先順序、Session 恢復、傳送背壓、腳本取消、鎖範圍、
資源銷毀順序、例外邊界與既有 API 保持原有政策；未進行錯誤介面遷移。
測試按情境拆分，原有斷言診斷文字比對保留；這項靜態比對不取代行為測試。

- Windows x64 MSVC Debug／Release：完整建置與 SDK 個別標頭編譯成功。
- Shell Socket／Tcl／Unit／Terminal／Integration／Console、SDK、Module／Message Bus
  中央程式測試：Debug **9/9** 通過（16.74 秒），Release **9/9** 通過（16.03 秒）。
- 驗證包含真實 TCP 的輸入分段／拒絕、Session 恢復與歸屬、8 條連線容量、IPv6、
  慢客戶端、遠端關機等待模組退出與尾端輸出，以及 Tcl 取消與 Console 控制事件。
- Linux／macOS 本輪未驗證，PuTTY GUI 未實測；未重跑其他不相關元件的完整測試。
- 當時仍有路徑轉型疑慮及 InputPipe 建構失敗回收待辦；後續狀態見下方修正紀錄。

## 複掃缺口修正與驗證（2026-10-04）

- `tests/terminal_tests.cpp`、
  `tests/tcl_tests.cpp`、`tests/socket_shell_tests.cpp`、`tests/integration_tests.cpp`、
  `tests/console_tests.cpp` 前次列出的 34 處非零等待／延遲值已依正常操作、刻意逾時、
  取消、觀察及清理用途命名，chrono 後綴保留時間單位。測試常數留在測試內。
- 將新常數展開後，與本輪修改前快照逐 token 比對一致：數值、等待位置、操作與斷言
  順序不變。前輪 clang-format 設定重跑後亦保持一致。
- Windows x64 Debug／Release 完整建置成功，七個 `osal.*` 與六個 `shell.*` 測試
  兩種組態各 **13/13** 通過。Linux／macOS 未執行。
- 正式實作的 `wait_for(0ms)` 是非阻塞就緒檢查，維持原樣；當時的路徑轉型與 InputPipe 待辦已於後續工作修正，見下方紀錄。

## 共用錯誤介面

Shell 使用 `vinc::Result<T>`／`vinc::Error`，錯誤與操作 enum 由 `<vinc/shell/error.hpp>` 維護。
ModuleSystem、OSAL 與 OutputHub 失敗在本層轉換並保留有界原因鏈；文字統一由
`error_to_string()` 查詢。Shell metadata provider 不依賴 Tcl runtime。

- `Shell::create()`、`SocketShell::create()`、`initialize_tcl()` 與網路啟動為內部 Result 入口。
- `process_line()` 的成功 value 保留應用命令回傳碼；非零 hook 回傳碼並非框架傳輸錯誤。
- 命令／腳本失敗顯示診斷後返回操作迴圈，保留框架與其他模組。
- Socket 執行中故障按連線、listener、worker 分層隔離，不單獨要求全框架關閉；
  退避、停用與診斷節流見 [Socket Shell 故障政策](SOCKET.md#容量與生命週期)。
- 本機 terminal 不可恢復故障由 `run()` 在完整關閉後回報；明確 exit／Ctrl+C 仍完整收尾。
- 啟動期缺少 Tcl 資源或 bind/listen 失敗保持既有啟動失敗政策，腳本尚未執行。
  這與服務運作後的網路故障隔離不同，不將所有錯誤視為中央程序致命故障。
- 既有標準函式庫、第三方及應用例外邊界維持；未全面停用 C++ exceptions。

`ScriptReport` 分開保存 Result 結果與有界 Tcl 檔名／行號／文字診斷。框架錯誤的數值原因鏈
放在 Tcl return options：`catch` 可以處理它，`return -options` 可重新傳遞；後來發生的
一般 Tcl 錯誤不會誤用先前已處理的框架錯誤。非法 options 回到一般腳本失敗，不信任任意數值。
命令橋接使用保存的原生命令 token，腳本 rename 原始命令後亦不會遞迴呼叫錯誤的名稱。

## 後續修正紀錄

- UTF-8 路徑：腳本路徑與 Tcl executable 路徑使用 OSAL 的嚴格轉換；不猜字碼頁，
  不再把 char 儲存重解釋為 char8_t view。Unicode 路徑限制見 OSAL 與中央入口文件。
- InputPipe：建構失敗先回收已取得的原生 handle／fd；測試 `shell.faults` 有故障注入
  與 stdin／資源回復檢查。此處屬測試輔助物件，沒有改變正式 stdin 所有權政策。
- Socket 故障隔離與 Tcl rename／return-options 的測試分別位於 `shell.faults`、`shell.tcl`。
  本輪 SDK 的實際重建與測試結果另列，不以舊歷史紀錄冒充本輪驗證。

## Module 契約故障顯示（2026-10-05）

本機與 SocketShell 在原有收集迴圈檢查 ModuleManager 的診斷 revision；變更時顯示
`[Module fault]`、名稱、MID、PID、實際退出 code／signal、中央是否曾要求 OS 終止，
以及原因、適用的 index／size、框架偵測 file／line／function。沒有新增診斷 thread。
Module 的 `output off` 不關閉中央故障通知；`help` 也顯示已保留的最後一筆。

子程序文字在顯示前替換 C0／DEL（含 ESC），避免嵌入終端控制序列；截短文字明確標示。
不完整或非法共享紀錄只顯示對應錯誤，不解讀部分欄位。診斷不宣稱中央已殺掉模組，
而是分開列出已確認的 OS 退出及是否曾發送終止要求。
`fatal exit deadline expired` 另行顯示中央觀察到有效退出期限已到，不表示 OS 終止請求成功。

每個 Session 各自持有通知 cursor，正常收集不重複顯示同一 revision；遠端斷線或背壓仍適用
既有 SocketShell 政策，沒有 TCP 畫面送達保證。中央保留範圍與故障邊界見
[ModuleSystem](../module_system/README.md#契約故障共享紀錄)。

本輪已在 Windows Debug／Release 驗證本機與真實 TCP 的通知、help 重查及 output off 行為；
完整結果見 [ModuleSystem 驗證](../module_system/README.md#共享故障紀錄驗證)。Linux／macOS 未驗證。
## 待處理事項

- 遠端背景輸出期間的輸入列重繪、密碼登入維持延後決策，見根目錄 TODO 與 SOCKET。
- Linux／macOS 的原生建置、部署、取消與退出行為仍待驗證；PuTTY GUI 亦未實測。
- 原生 Tcl 擴充或阻塞操作可能延遲取消，不承諾強制中止中央執行緒。

## SDK 邊界與文件恢復（2026-10-06）

本頁先前被 Message Storage 主體覆蓋。本次以本機保留的 Shell 重工前快照為文字來源，
逐段核對現行 Result、嚴格路徑轉換、故障隔離及使用者的自動啟動決策；保留真正屬於
Shell 的 Module 故障顯示說明，移除誤置的 MessageArray 契約及已過時的 singleton 說明。
中央自行準備預設服務；Shell／SocketShell 接線標頭移到 detail，既有執行順序與政策不變。
本輪驗證結果見 [SDK](../sdk/README.md#sdk-重審修正2026-10-06)。

## Module 除錯命令

- `debug start <ModuleType> [args...]`：等待 Host 載入前 gate，顯示 MID、PID、Plugin 路徑。
  Module argv 沿用一般啟動語意，第一個元素為 ModuleType。取得 gate 的等待最多為既有
  `config::shell_command_timeout`；逾時／腳本取消不停止已建立的 Host，使用 help 查閱並停止。
- `debug continue <MID>`：排入啟動放行，立即返回；不等應用 Ready，不操作 debugger 本身。
- `debug stop <MID>`：使用既有有界 Module 停止流程，也能處理尚未註冊名稱的 Host。
- `help` 在 Modules 列表標示 DEBUG 與載入前等待。`debug` 成為保留命令名稱。
- 本機 debug 命令走既有非同步工作，避免等待阻塞 Ctrl+C；Socket Shell 與 Tcl 共用 router。
  明確 Ctrl+C／exit 對除錯 Module 仍執行完整 shutdown。

步驟及 debugger／符號限制見 [Module 除錯指南](../module_system/DEBUGGING.md)。

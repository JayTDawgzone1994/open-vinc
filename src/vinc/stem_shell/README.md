# 互動式 Shell

Shell 是中央的操作介面，模組命令仍經 `ModuleManager → control Socket → BaseModule virtual hook`。
訊息 Bus payload 不經 Shell 或 Socket 傳遞。

## 分工與所有權

- `main.cpp`：建立並持有 ModuleManager，登記 CMake 模組目錄，選擇原有 `-sc` 入口或互動介面。
- `CommandRouter`：Shell 自己的 `help`／`exit` 註冊表、命令派送、完整錯誤與補全結果。
- `command_line`：引號與參數解析、128 筆歷史、4096-byte 輸入上限與 UTF-8 行尾編輯。
- `StemShell`：終端事件迴圈、命令工作與補全結果顯示、完整關閉流程。
- OSAL `Terminal`：Windows console／POSIX terminal 的模式管理、按鍵、UTF-8、EOF、SIGINT／Ctrl+C。
  終端設定只在進入互動迴圈時啟用，離開時還原。

`StemShell(manager, output)` 明確借用 manager 與輸出；它們必須比 Shell 活得久。
預設建構子保留 singleton 相容入口。Shell 的 UI／process_line／生命週期操作由同一控制執行緒使用，
不可與另一個 `run`／析構並行。模組工作在框架內部的工作執行緒進行。
全程序同時只允許一個 OSAL Terminal 擁有標準輸入，第二個 open 回 Busy。

ModuleManager 只保留 `help`、`exit` 的命名保留，直到 manager 析構；不持有指向 Shell 的 callback。
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

第一次 Ctrl+C、`exit` 或輸入 EOF 都啟動同一流程；重複 Ctrl+C 不跳過清理，也不直接 `_Exit`：

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

## 腳本範圍

`ScriptInterpreter` 的語言、變數、條件與迴圈未在這次重構。
保留同步 `process_line` 供原有腳本使用；必要入口銜接會檢查載入失敗及 `-sc` 缺少參數，
並保留失敗退出碼。不要將本次互動式 Ctrl+C 驗證解讀成已修復腳本迴圈與數值解析。

## 驗證

- `shell.unit`：引號／空字串／跳脫、Windows 路徑、補全 roundtrip、中文刪除、歷史草稿、
  Shell 重建的 callback 生命週期、命名保留、原生回傳碼與候選清單。
- `shell.terminal`：重導向 UTF-8 分段、CRLF、方向鍵、孤立 Escape、Ctrl+C、非法 UTF-8、EOF、獨佔與釋放。
- `shell.integration`：真實 Module 程序的參數與補全，命令逾時後 Busy／遲到回覆，
  卡住命令期間仍操作其他模組；Ctrl+C 等待正常清理並強制終止卡住程序，確認退出；exit／EOF 共用流程。
- Windows 的 `shell.console`：在獨立隱藏 console 注入原生鍵盤事件，驗證中文、代理對、Ctrl+C、
  方向鍵、Backspace，以及 input/output mode 與 code page 還原；另外實際廣播 OS CTRL_C_EVENT，
  確認中央收到事件，模組經框架完成 `on_stop()` 並以正常退出碼結束。不是人工實機操作紀錄。

本次在 Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2
完成 Debug 與 Release 建置：各 19/19 套 CTest 通過，包含以上四套 Shell 測試與既有回歸測試。
Linux／macOS 後端尚未進行原生建置與行為驗證。

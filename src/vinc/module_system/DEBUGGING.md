# Module 除錯指南

Module DLL 由中央啟動的獨立 ModuleHost 載入。除錯器附加到該 Host 的 **OS PID**，
Shell 命令則以 **MID** 選擇 Module；兩者不能互換。

## 使用流程

先使用同一組態建置框架、ModuleHost 與 Plugin。Windows 建議使用 MSVC Debug，
現有建置會產出 Host 與 Plugin 的 PDB；保留匹配的 DLL／EXE、PDB 與原始碼。
Release 的最佳化及符號可用性依其建置設定，不保證能逐行對應原始碼。

在 Vinc Shell：

```text
debug start exampleModule --name camera
```

回覆包含 `MID=... PID=... Plugin=...`。此時尚未載入 DLL，也尚未執行 Module 建構／命名 hook。

1. 將 debugger 附加到回覆的 PID，而不是中央 Vinc.exe。
2. 載入匹配的符號，設定 Module 建構子、`on_start()` 或其他位置的斷點。
3. 在 debugger 中繼續執行，讓 Host 能接收控制訊息。
4. 回到 Shell，使用回覆提供的 MID：

```text
debug continue 123
```

這只放行啟動 gate，不操作 debugger，也不等待 Module 就緒。用 `help` 查看 Running 狀態；
Module 有名稱後，既有 Shell 命令照常使用。附加除錯不強制修改 Module 的原始碼或入口。

停止時可使用：

```text
debug stop 123
```

Ctrl+C／exit 仍完整關閉中央及其所有 Module；即使停在斷點，也會依停止期限請 OS 終止。

## 各平台工具

- Windows＋MSVC：Visual Studio 使用 Attach to Process，選擇原生程式碼，核對 PID；
  Modules 視窗檢查 DLL 的符號是否載入。[官方附加說明](https://learn.microsoft.com/en-us/visualstudio/debugger/attach-to-running-processes-with-the-visual-studio-debugger?view=visualstudio)
- Linux＋GDB：`gdb -p <PID>`；尚未載入 DLL 時可用 `set breakpoint pending on`，
  再 `break <函式名稱>`、`continue`，最後由 Shell 放行。
- macOS＋LLDB：`lldb -p <PID>`，設定 `breakpoint set --name <函式名稱>`，再 `continue`。
  附加權限依 OS 與開發工具設定。[LLDB 官方說明](https://lldb.llvm.org/use/tutorial.html)

Windows 自動測試會透過原生 Debug API 附加真正 Host、停在 breakpoint、繼續並 detach。
一般原生建置與測試結果見[平台驗證報告](../../../README.md#validation-and-limits)。
外部 debugger 的人工操作另計：Visual Studio GUI／PDB 原始碼斷點，以及 Linux GDB／macOS LLDB 的實際附加操作，仍無本次人工驗證紀錄。

## 政策與限制

除錯模式在每次啟動時明確選擇，與 Debug／Release 組態無關；只作用於指定 Module。
該次啟動不因 heartbeat／startup／fatal 自動期限被殺掉，也不會自動重啟。
真實錯誤、程序退出仍記錄；沒有 debugger 時也不會自動附加或暫停在故障點。
要查看契約違反可事先對 `vinc::contract_violation` 設斷點，繼續後仍遵循 fatal 退出路徑。

以下限制仍然存在：

- 必須等載入前 gate 回覆才附加，Host bootstrap／connect／handshake 仍使用有界等待。
- DLL 靜態建構子早於 RuntimeServices 注入，該階段的故障仍可能只有 fallback termination。
- debugger 暫停可能凍結所有 Host thread。命令／Bus 請求可能逾時；佇列滿載、協定錯誤、
  真實連線中斷仍能導致故障，沒有暫停後所有操作透明續跑的保證。
- 暫停時若仍持有共享 Reader 保護，其他 Publisher 可能等待。不能提前回收尚存活程序的 lease。
- 不放寬明確 stop／shutdown 的期限，因此不能無限停在 `on_stop()` 裡。
- detach 不自動恢復一般監督；停止後正常啟動才恢復。真的卡死時需明確停止。
- 不提供脫離中央的獨立 Module 執行環境、crash dump 管理或內建 debugger。

完整 API 與協定契約見 [Module System README](README.md#module-除錯支援2026-10-06)。

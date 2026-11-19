# Single application entry / 單一應用程式入口

The framework supplies one production executable for both central and native Module
Host roles. Each Module still runs in a separate process and loads its own Plugin.
框架提供唯一正式入口；同一份執行檔依角色啟動中央或 Module Host，程序隔離不變。

## Entry and lifecycle / 入口與生命週期

- Users implement `BaseModule`, including its business `main()` when needed; they
  do not supply a replacement application or Host `main()`.
- Role selection happens before central command parsing or service initialization.
  Host mode does not start another central instance, Tcl, Socket Shell or startup script.
- The internal bootstrap marker is versioned. Invalid or incompatible bootstrap
  input is rejected; it must not fall back to central mode. A marker is not authentication:
  the existing central handshake and startup deadline still apply before Plugin work.
- The central process obtains its executable path through OSAL and launches that
  file, independent of the current directory or `argv[0]` spelling.
- Module System owns Plugin loading, watchdog and runtime services. Plugin creation
  and destruction remain paired inside the Plugin; no cross-DLL allocator ownership changes.

使用者只實作 Module，不另寫程序入口。Host 不重複啟動中央服務；錯誤的內部啟動參數
直接拒絕，不能誤入中央。相同工具鏈、CRT、標準庫與布局要求仍然適用。

## Build and deployment / 建置與部署

Source builds and installed SDK consumers both use `vinc_add_app()`.
There is no production standalone `ModuleHost` target, export or deployed executable.
Test-only Hosts and build tools such as Ribosome do not change this deployment rule.
Python Modules still run in the selected external CPython interpreter.

完整 App 位於 `<build>/output/<App>/<configuration>/`；內建 App 的 CMake 工作資料
位於 `<build>/intermediate/vinc/`。部署內容與命令見 [App README](README.md)
及 [SDK 組裝指南](../../src/vinc/sdk/INSTALL.md#c-application-composition--c-應用組裝)。
合併入口不代表改用 fork、共用 Module 記憶體，或承諾降低每個程序的成本。

## Validation / 驗證

- [Windows／WSL 原始驗收](SINGLE_EXECUTABLE_VALIDATION.md)：來源版、乾淨 SDK、搬移與失敗案例。
- [macOS 最新來源／SDK 驗收](../../src/vinc/sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)：實際版本、測試範圍與限制。
- [多 App 契約](MULTI_APP.md)：組裝與實例隔離。

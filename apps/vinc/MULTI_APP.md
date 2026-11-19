# Multiple App composition / 多 App 組裝契約

One project can register shared Modules/messages and compose multiple independent
Apps. Source builds and installed SDK consumers use the same constructor.
一個專案可共用 Module／Message，組成多個有自己設定、腳本與部署目錄的 App。

```cmake
vinc_add_app(CameraStation
    INSTANCE_NAME camera-station
    VINC_BUILD_CONFIG camera-station.config
    STARTUP_SCRIPT startup.tcl
)
```

## Composition / 組裝

- Each App selects registered Modules through its Kconfig file. User Modules are
  not selected implicitly; unknown selections are errors. Generated symbols must
  be stable and collision-checked.
- UI and Bag Recorder are provided as official System Modules. Python is optional
  and disabled by default. Runtime activation is separate from build selection.
- Relative config/script paths resolve against the caller's CMake source directory;
  explicitly requested missing files fail configuration. An embedded startup script
  cannot be replaced with the runtime `-S` option.
- Each App owns its catalog, generated configuration, startup script and deployment
  manifest. Compatible native Module targets compile once and are copied into each
  App that selects them. UI retains a thin App-specific Plugin target for its resource path.
- Messages share the build's registered schema set; there is no per-App schema pruning.
  Python-enabled Apps share one binding and must resolve the same build interpreter.
  Different ABIs require separate build directories.

各 App 的設定與部署獨立；共用編譯產物仍須符合 ABI。未選入 Module 不需要建置，
但框架不會替使用者跳過其 CMake 已執行的相依搜尋。Python 選擇方式見
[設定工具](../../tools/configure/README.md)。

## Startup and service ownership / 啟動與服務所有權

1. Admit the central instance and initialize its services, including Param and Log.
2. Register ModuleTypes and start official UI/Recorder; wait for Ready or failure.
3. Run the App's startup script, with failures explicitly reported.

Official-Module failure does not terminate the central process or unrelated Modules.
System Module supervision and retry limits follow the
[Module System policy](../../src/vinc/module_system/README.md#system-modules).
Users control UI/recording through Shell commands, not ordinary Module stop commands.
UI disable stops its interface; it does not remove the protected System Module.
Recorder starts idle and does not resume recording automatically after a restart.

先完成官方 Module 的啟動結果，再執行 Tcl。某個服務失敗時仍繼續提供其他服務；
腳本若使用不可用功能，會收到錯誤。參數是否需要重啟依其註冊契約，不能假定
在 startup.tcl 修改的值會重新套用到已初始化的服務。

## Identity, paths and isolation / 身分、路徑與隔離

- The same `INSTANCE_NAME` cannot run twice in the applicable user/OS identity scope.
  Different names can run concurrently, with separate control endpoints and owned resources.
- Each complete App lives in `<build>/output/<App>/<configuration>/`.
  The bundled App's CMake workspace is `<build>/intermediate/vinc/`.
- Deployment manifests retire only files they own. Do not delete user data or another App's files.
- Different instance names do not reserve different TCP ports. Current defaults are
  Socket Shell `127.0.0.1:31410` and UI port `31411`; concurrent Apps need compatible
  listener settings. Shell listener failure aborts startup; UI listener failure is
  isolated to its System Module.

同名排他與不同 App 的資源隔離，不代表 TCP port 自動分配。
網路設定與套用時機見 [Network parameters](../../src/vinc/shell/NETWORK_PARAMS.md)。

## Validation / 驗證

- [Windows／WSL 原始組裝及隔離驗收](MULTI_APP_VALIDATION.md)。
- [macOS 最新來源、乾淨 SDK、兩 App 與搬移驗收](../../src/vinc/sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)。
- [SDK 使用方式](../../src/vinc/sdk/INSTALL.md)。

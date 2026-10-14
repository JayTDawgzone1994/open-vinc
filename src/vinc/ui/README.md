# Web UI

第一版提供官方 `webUi` Module、瀏覽器工作區、分頁生命週期及 UI plugin SDK。
預設建置並登記 ModuleType，**不會隨中央自動啟動**；預設監聽 `127.0.0.1:8080`。
瀏覽器不需要 Node.js。中央、ModuleHost 與一般 Module SDK 不連入 HTTP／JSON 實作。

目前沒有 Param、Log、Module 管理、Topic／訊息、流量圖或 3D 面板，也沒有管理 RPC。
它們會是後續 plugin 與中央管理接線的工作；不是目前範例畫面假裝已提供的能力。

## 建置與啟動

前端需 Node.js（本機驗證 24.19.0）與 npm。相依版本及 integrity 固定於
`web/package-lock.json`；CMake 不自行執行 npm install 或下載套件。

```sh
git submodule update --init --recursive
npm ci --prefix src/vinc/ui/web
cmake -S src -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
```

不建置 UI 可指定 `-DVINC_BUILD_UI=OFF`。`VINC_BUILD_APPS=OFF` 的純 SDK 使用端也不需要
Node、npm 或 UI 的三個原生相依。建置仍需符合主專案 C++23、Tcl 與 Plugin ABI 契約。

在 Vinc Shell 或 Tcl 執行：

```tcl
webUi
ui status
```

用瀏覽器開啟 `http://127.0.0.1:8080/`。需要變更時，在啟動命令指定：

```tcl
webUi --ip 127.0.0.1 --port 8090
```

`--ip` 接受數字 IPv4／IPv6 或 `localhost`（轉為 127.0.0.1），不做 DNS 猜測。
`--port 0` 由 OS 分配並印出 URL；非 loopback 位址須明確指定。現在只服務靜態 UI 與套件清單，
沒有登入、HTTPS 或遠端管理保證。改成對外監聽會開放這些套件內容給連入者。

預設實例名稱 `ui`，沿用 Module System 的重名拒絕規則。
停止使用既有 Shell 命令 `debug stop <MID>`（啟動輸出及 `help` 顯示 MID）；
這個命令同樣可停止非 debug 啟動的 Module。中央正常退出亦會停止它。停止後可重新執行 `webUi`。

## 部署

```text
<build>/vinc_data/<VINC_NAME>/ui/
├── app/                    # Vite 生成的工作區與 third-party.txt
└── plugins/
    ├── vinc.example/       # 官方範例，建置管理
    └── your.plugin/        # 使用者安裝的套件
        ├── plugin.json
        ├── index.js
        └── style.css
```

預設 root 在建置時固定為上列**絕對路徑**，不依使用者當前 cwd。搬移部署可用
`webUi --root /absolute/path/to/ui` 指定完整的 app＋plugins 目錄。
這版沒有完整可重定位 install/package 規則；中央自己的 Module catalog 路徑契約仍適用。

`vinc_ui_deploy` 更新建置擁有的 `app`、`plugins/vinc.example`，移除其中舊生成檔；
不碰其他 plugin 目錄。不要直接修改這兩個生成目錄保存自己的作品。
複製部署時一併帶上 `app/third-party.txt`，保留 MIT 授權與聲明。

新增／修改 plugin 後，停止並重啟 UI Module，再重新整理瀏覽器即可。
不需要重啟中央或其他 Module；第一版不做熱卸載 JS、分頁狀態保存或 plugin 套件下載器。
伺服器重啟後清單改變，既有頁面會提示 refresh，不把新舊套件混裝到同一工作區。

## 元件與相依

```text
中央 ModuleManager ──啟停／heartbeat── ModuleHost
                                      └── vinc_module_ui (native Plugin)
                                           └── vinc_ui
                                                ├── common / OSAL 路徑與位址
                                                └── 私有 cpp-httplib / json / PicoSHA2
瀏覽器 ──HTTP GET── UI Module
  └── Vue 工作區 ──DOM＋PanelContext── UI plugin (ES module)
```

- `vinc_ui` 擁有一次啟動的不可變資源快照及 HTTP service；`Server::start()` 回傳 Result，
  owner 串行啟停／析構，`running()` 可並行查詢。不接中央 registry 或管理權限。
- cpp-httplib 自行處理其原生 sockets，透過正式設定 API 封裝，未修改上游；
  Windows 排他綁定、POSIX reuse-address，不使用套件預設的 reuse-port 行為。
- 1 個 listener 執行緒、固定 2 個 HTTP worker，最多 16 個等待工作。沒有自動擴充 worker。
  ModuleHost 既有 heartbeat／watchdog 的責任不變。HTTP listener 異常結束會由 `ui status`
  顯示 `running=0`、瀏覽器顯示斷線；目前需手動停止／重啟 UI Module。
- `ErrorDomain::Ui = 13`，固定錯誤與操作碼由 UI 維護；失敗文字經 common 的靜態 catalog。
  HTTP 回覆只使用 status／一般說明，不暴露伺服器原生路徑或 exception 文字。
- HTTP handler 不接觸 Context，不從 worker 呼叫使用者 Module callback。

## 資源、失敗與信任契約

- 啟動時將 app／plugins 讀成固定記憶體快照；HTTP 僅依精確 route 讀此快照，不逐次開檔。
  啟動失敗由 Module Result 回報，不終止中央。無效 plugin 列診斷並跳過；重複 ID 的套件全部拒絕。
- 路徑只接受 ASCII 字母數字、`_-. /` 組成的相對路徑；拒絕 `..`、symlink 與特殊檔案。
  部署 root 路徑支援嚴格 UTF-8。這不是對抗有權同時修改本機部署目錄者的 filesystem sandbox。
- SHA-256 包含套件所有檔案（包括 manifest），按相對路徑排序；逐筆輸入
  `path + NUL + decimal(byte length) + NUL + bytes`。URL 含 digest，服務的是同一份已計算快照。
- 官方範例的 ID＋digest 必須與建置時嵌入值相同才預信任；套件自行宣告 `official` 無效。
  外部 plugin 第一次執行前顯示信任視窗，拒絕時不 import；同意以 browser origin＋ID＋digest 保存。
  localStorage 無法使用時只記住當前頁面。變更內容重新詢問；清除瀏覽器網站資料可撤銷記憶。
- **信任 plugin 代表允許它在工作區執行程式。** Shadow DOM 只隔離一般 CSS；不是權限或執行隔離。
  它可以接觸 window／同 origin 內容；無限迴圈仍可能卡住整個頁面，workspace 不能強制回收其任意資源。
- CSP 限制 script／connect 同 origin，禁嵌框架；驗證 Host／Origin，拒絕跨站請求，無 CORS 開放。
  這些不等同登入，更不等同中央的管理授權。批准 plugin 的按鈕不是中央提權介面。
- 開關分頁保留各自狀態；關閉先 abort signal、執行已登記 cleanup，再呼叫 dispose。
  同步 lifecycle 失敗顯示於該面板；plugin 自行啟動的非同步工作仍須自行處理失敗與取消。
  動態 import 不能取消或真正卸載，已批准的 JS 可能在面板關閉後才完成 import；不再建立該面板。
- 連線只輪詢唯讀 catalog：斷線不銷毀既有面板，每 5 秒再試，單次 3 秒上限。

設定在 `vinc/config.hpp`（前端在 `web/workspace/runtime.ts`）：

| 設定 | 預設／行為 |
| --- | --- |
| ui_port | 8080 |
| ui_file_bytes / ui_snapshot_bytes | 單檔 8 MiB／啟動讀入累計 64 MiB；不包含索引、allocator、HTTP 回覆副本 |
| ui_package_entries / ui_plugin_limit | 每套件 1024 個目錄項目／最多 64 個 plugin 目錄項目 |
| ui_manifest_bytes / ui_panels_per_plugin | 16 KiB／每套件最多 16 個 panel types |
| ui_identifier_bytes / ui_label_bytes | ID 80 bytes／manifest 字串欄位 128 bytes |
| ui_http_workers / ui_http_queue | 2／16；滿載由 HTTP library 關閉超額連線 |
| ui_http_timeout | 讀／寫等待 2 秒，不是整個 HTTP 請求的硬性總期限 |
| ui_start_timeout / ui_start_poll_interval | listener 就緒觀察 5 秒／5 ms |
| ui_stop_timeout | Module 正常停止預算 10 秒，逾時由中央／Host 既有政策終止該 Module |
| browser limits | 最多 24 分頁、64 筆批准記錄；標題 80 字元、通知 240 字元 |

HTTP 只接受 GET／HEAD，不接受 body，不提供 upload／管理 endpoint。慢客戶端及已排隊請求
可能延長 worker join；2 秒是等待 timeout，不承諾優雅停止必然在 10 秒內完成，必要時依 Module 政策強制退出。

## Plugin 開發

見 [PLUGIN.md](PLUGIN.md)，最小範例原始碼位於 `web/plugins/example/`。
SDK 為 `web/sdk/index.ts`，不要求 plugin 使用 Vue，也不暴露 Vue app/router/store。

## 驗證

```sh
cmake --build build/debug --target vinc_ui_tests vinc_ui_test_server vinc_ui_module_tests shell
ctest --test-dir build/debug -R "^ui\." --output-on-failure
cd src/vinc/ui/web
npm run typecheck
npx playwright install chromium
# VINC_UI_BUILD 指向欲測試的絕對 build 目錄；Windows 本機預設為 windows-msvc-debug。
npm test
```

本機 2026-10-14：Windows x64、MSVC 19.51、Node 24.19.0。
Debug／Release 的 `ui.server`、`ui.module`、`ui.shell` 均通過。
Chromium 四組瀏覽器案例與 Vue／TypeScript 型別檢查通過，檢視桌面與手機寬度截圖。
Debug 既有 `module.shell`、`message_bus.shell`、`bag.shell` 亦通過。
另以 APPS／EXAMPLES／PYTHON／TESTING 關閉的純 SDK 配置確認沒有 UI／Node／HTTP 相依。
Linux／macOS 尚未實機驗證，不宣稱已通過。

## 待處理事項

- **已確認的下一階段**：官方管理面板與中央管理 bridge。須由中央核准官方 UI 的 MID／session，
  不能以 `ui` 名稱或 plugin 自稱官方取得權限；一般 Module 的 Param 權限維持現況。
  加入任何管理 endpoint 前先設計 browser session／登入、CSRF／Origin 與命令授權。
- **未實作**：管理資料串流、官方 feature plugins、可重定位安裝包、Electron 包裝；本輪不做 placeholder RPC。
- **驗證限制**：原生 Linux／macOS、多瀏覽器及大量慢連線負載測試待執行。
  目前 HTTP thread 故障由 status／瀏覽器顯示，尚無自動轉換為 Module 故障的監督接線。

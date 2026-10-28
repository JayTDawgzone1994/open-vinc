<a id="web-ui"></a>

# Web UI / 瀏覽器工作區

The official webUi Module provides a browser workspace, tab lifecycle, and plugin SDK. It is built/registered by default but **never automatically started**. Default listener is 127.0.0.1:8080. Browsers need no Node; central/Host/general Module SDK do not link HTTP/JSON implementations.

第一版提供官方 `webUi` Module、瀏覽器工作區、分頁生命週期及 UI plugin SDK。
預設建置並登記 ModuleType，**不會隨中央自動啟動**；預設監聽 `127.0.0.1:8080`。
瀏覽器不需要 Node.js。中央、ModuleHost 與一般 Module SDK 不連入 HTTP／JSON 實作。

Official vinc.topics provides read-only topic lists, publication Hz, collision/loss diagnostics, and latest-message previews. Official vinc.graph adds a manually refreshed Module/Topic relationship view. Param/Log/Module management, 3D panels, and central administration RPC remain unwired. Topic Monitor uses ordinary read-only Context Bus services, without parameter-write or Module-control authority.

已有官方 `vinc.topics` Topic Monitor：唯讀 Topic 清單、發布 Hz、碰撞／漏資料及最新訊息預覽。
官方 `vinc.graph` 提供手動刷新的 Module／Topic 關係圖。
仍沒有 Param、Log、Module 管理或 3D 面板，也沒有中央修改／管理 RPC。
Topic Monitor 使用一般 Context 的唯讀 Bus 能力，不取得修改參數或啟停其他 Module 的權限。

<a id="建置與啟動"></a>

## Build and start / 建置與啟動

Frontend builds need Node/npm (locally verified 24.19.0). package-lock pins versions/integrity. CMake never runs npm install or downloads packages.

前端需 Node.js（本機驗證 24.19.0）與 npm。相依版本及 integrity 固定於
`web/package-lock.json`；CMake 不自行執行 npm install 或下載套件。

The optional [dependency setup scripts](../../../tools/README.md) prepare Node/npm
and run `npm ci` for you, without configuring or building a project.

可使用 [相依環境準備腳本](../../../tools/README.md) 安裝／檢查 Node／npm 並執行 `npm ci`；
腳本不配置或編譯專案。

```sh
git submodule update --init --recursive
npm ci --prefix src/vinc/ui/web
cmake -S src -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
```

Use VINC_BUILD_UI=OFF to omit it. SDK-only VINC_BUILD_APPS=OFF consumers need neither Node/npm nor the three UI native dependencies. C++23/Tcl/Plugin ABI contracts still apply.

不建置 UI 可指定 `-DVINC_BUILD_UI=OFF`。`VINC_BUILD_APPS=OFF` 的純 SDK 使用端也不需要
Node、npm 或 UI 的三個原生相依。建置仍需符合主專案 C++23、Tcl 與 Plugin ABI 契約。

In Vinc Shell or Tcl:

在 Vinc Shell 或 Tcl 執行：

```tcl
webUi
ui status
```

Open http://127.0.0.1:8080/. Override listener settings at Module launch:

用瀏覽器開啟 `http://127.0.0.1:8080/`。需要變更時，在啟動命令指定：

```tcl
webUi --ip 127.0.0.1 --port 8090
```

--ip accepts numeric IPv4/IPv6 or localhost (127.0.0.1), without DNS guessing. --port 0 prints an OS-assigned URL. Nonloopback requires explicit configuration. Current endpoints serve assets/catalog and capability-checked read-only diagnostics, with no user login, HTTPS, or remote-administration guarantee. Public listeners expose assets/diagnostics to connected clients; the capability does not block native clients able to connect.

`--ip` 接受數字 IPv4／IPv6 或 `localhost`（轉為 127.0.0.1），不做 DNS 猜測。
`--port 0` 由 OS 分配並印出 URL；非 loopback 位址須明確指定。現在服務 UI、套件清單及經 browser capability 驗證的唯讀 Topic 診斷，
沒有使用者登入、HTTPS 或遠端管理保證。改成對外監聽會讓連入者取得套件及唯讀診斷；browser capability 不阻止能直接連入的原生客戶端。

Default instance ui follows duplicate-name rejection. Stop with debug stop <MID> (MID shown at startup/help), also valid for non-debug Modules. Central orderly exit stops it; afterward webUi may restart.

預設實例名稱 `ui`，沿用 Module System 的重名拒絕規則。
停止使用既有 Shell 命令 `debug stop <MID>`（啟動輸出及 `help` 顯示 MID）；
這個命令同樣可停止非 debug 啟動的 Module。中央正常退出亦會停止它。停止後可重新執行 `webUi`。

<a id="部署"></a>

## Deployment / 部署

```text
<build>/vinc_data/<VINC_NAME>/ui/
├── app/                    # Vite 生成的工作區與 third-party.txt
└── plugins/
    ├── vinc.example/       # 官方範例，建置管理
    ├── vinc.graph/         # 官方 Module Graph，建置管理
    ├── vinc.topics/        # 官方 Topic Monitor，建置管理
    └── your.plugin/        # 使用者安裝的套件
        ├── plugin.json
        ├── index.js
        └── style.css
```

Default root is the build-fixed **absolute** path above, independent of cwd. Relocation can use webUi --root /absolute/path/to/ui for complete app+plugins. No complete relocatable install/package exists; central catalog path limits still apply.

預設 root 在建置時固定為上列**絕對路徑**，不依使用者當前 cwd。搬移部署可用
`webUi --root /absolute/path/to/ui` 指定完整的 app＋plugins 目錄。
這版沒有完整可重定位 install/package 規則；中央自己的 Module catalog 路徑契約仍適用。

vinc_ui_deploy replaces build-owned app/plugins/vinc.example/plugins/vinc.topics/plugins/vinc.graph including stale generated files, never other plugin directories. Do not save custom work there. Carry the complete app directory when deploying: app/third-party.txt combines Open-VINC's terms/attribution, the shared dependency inventory, and installed frontend licenses. The three shared files are also downloadable under app/licenses/open-vinc/. Native Module binaries follow [SDK license delivery](../sdk/README.md#license-delivery).

`vinc_ui_deploy` 更新建置擁有的 `app`、`plugins/vinc.example`、`plugins/vinc.topics`、`plugins/vinc.graph`，移除其中舊生成檔；
不碰其他 plugin 目錄。不要直接修改這些生成目錄保存自己的作品。
部署時攜帶完整 `app`：`app/third-party.txt` 包含 Open-VINC 條款／署名、共用第三方盤點及實際前端相依授權；
`app/licenses/open-vinc/` 亦提供三個共用聲明檔供下載。原生 Module binary 依
[SDK 授權交付](../sdk/README.md#license-delivery) 部署。

After plugin changes, restart only the UI Module and refresh the browser, not central/peers. No hot JS unloading, persisted tab state, or package downloader. Changed server catalogs prompt refresh instead of mixing old/new packages.

新增／修改 plugin 後，停止並重啟 UI Module，再重新整理瀏覽器即可。
不需要重啟中央或其他 Module；第一版不做熱卸載 JS、分頁狀態保存或 plugin 套件下載器。
伺服器重啟後清單改變，既有頁面會提示 refresh，不把新舊套件混裝到同一工作區。

<a id="元件與相依"></a>

## Components and dependencies / 元件與相依

```text
中央 ModuleManager ──啟停／heartbeat── ModuleHost
                                      └── vinc_module_ui (native Plugin)
                                           └── vinc_ui
                                                ├── common / OSAL 路徑與位址
                                                └── 私有 cpp-httplib / json / PicoSHA2
瀏覽器 ──HTTP GET── UI Module
  └── Vue 工作區 ──DOM＋PanelContext── UI plugin (ES module)
```

- vinc_ui owns an immutable startup asset snapshot/HTTP service. Server::start returns Result; owner serializes lifecycle/destruction, running may be queried concurrently. No direct Context/central registry/admin authority; injected Topic cache exposes bounded JSON strings.
- Unmodified cpp-httplib owns native sockets via supported APIs: exclusive Windows binding, POSIX reuse-address, no default reuse-port.
- One listener, two fixed HTTP workers,16 queued jobs, no auto-scaling. Existing Host heartbeat/watchdog remain. Listener failure shows running=0/disconnection; currently stop/restart manually.
- UI owns ErrorDomain::Ui=13 identifiers/static catalog. HTTP sends status/generic messages, not native paths/exception text.
- HTTP workers never use Context or invoke user Module callbacks.

- `vinc_ui` 擁有一次啟動的不可變資源快照及 HTTP service；`Server::start()` 回傳 Result，
  owner 串行啟停／析構，`running()` 可並行查詢。不直接接 Context、中央 registry 或管理權限。Topic 快取由 UI Module 注入，只回傳有界 JSON 字串。
- cpp-httplib 自行處理其原生 sockets，透過正式設定 API 封裝，未修改上游；
  Windows 排他綁定、POSIX reuse-address，不使用套件預設的 reuse-port 行為。
- 1 個 listener 執行緒、固定 2 個 HTTP worker，最多 16 個等待工作。沒有自動擴充 worker。
  ModuleHost 既有 heartbeat／watchdog 的責任不變。HTTP listener 異常結束會由 `ui status`
  顯示 `running=0`、瀏覽器顯示斷線；目前需手動停止／重啟 UI Module。
- `ErrorDomain::Ui = 13`，固定錯誤與操作碼由 UI 維護；失敗文字經 common 的靜態 catalog。
  HTTP 回覆只使用 status／一般說明，不暴露伺服器原生路徑或 exception 文字。
- HTTP handler 不接觸 Context，不從 worker 呼叫使用者 Module callback。

<a id="資源失敗與信任契約"></a>

## Resources, failures, and trust / 資源、失敗與信任契約

- Startup snapshots app/plugins; HTTP serves exact routes from memory, without per-request files. Startup Result failure affects the Module, not central. Invalid plugins are diagnosed/skipped; all packages sharing duplicate IDs are rejected.
- Relative package paths allow ASCII alphanumeric/_-./ and spaces as specified, rejecting .., symlinks, special files. Root supports strict UTF-8. This is not a filesystem sandbox against authorized concurrent local mutation.
- SHA-256 covers every package file including manifest, sorted by relative path, feeding path+NUL+decimal(byte length)+NUL+bytes. Digest URLs serve that exact hashed snapshot.
- Official pretrust requires build-embedded ID+digest, not a self-declared official flag. Unknown code is not imported before first-use consent. Decisions bind browser origin/ID/digest; unavailable localStorage keeps only page-local consent. Changes reprompt; clearing site data revokes remembered consent.
- **Trust permits executing code in the workspace.** Shadow DOM isolates ordinary CSS, not permissions/execution. Plugins can access window/same-origin state; infinite loops can freeze the page and arbitrary resources cannot be forcibly reclaimed.
- Same-origin script/connect CSP, no framing, Host/Origin checks, and no CORS are not user login or central authorization. Plugin trust buttons confer no central authority.
- Tab visibility retains state. Closing aborts, runs registered cleanup, then dispose. Synchronous lifecycle errors stay panel-local; plugin async jobs handle their own cancellation/failure. Imports cannot cancel/unload and may finish after close, without creating that panel.
- Workspace polls read-only catalog with 5 s retry/3 s request timeout, retaining panels offline. Topic plugin separately polls diagnostics/previews.

- 啟動時將 app／plugins 讀成固定記憶體快照；HTTP 僅依精確 route 讀此快照，不逐次開檔。
  啟動失敗由 Module Result 回報，不終止中央。無效 plugin 列診斷並跳過；重複 ID 的套件全部拒絕。
- 路徑只接受 ASCII 字母數字、`_-. /` 組成的相對路徑；拒絕 `..`、symlink 與特殊檔案。
  部署 root 路徑支援嚴格 UTF-8。這不是對抗有權同時修改本機部署目錄者的 filesystem sandbox。
- SHA-256 包含套件所有檔案（包括 manifest），按相對路徑排序；逐筆輸入
  `path + NUL + decimal(byte length) + NUL + bytes`。URL 含 digest，服務的是同一份已計算快照。
- 官方套件的 ID＋digest 必須與建置時嵌入值相同才預信任；套件自行宣告 `official` 無效。
  外部 plugin 第一次執行前顯示信任視窗，拒絕時不 import；同意以 browser origin＋ID＋digest 保存。
  localStorage 無法使用時只記住當前頁面。變更內容重新詢問；清除瀏覽器網站資料可撤銷記憶。
- **信任 plugin 代表允許它在工作區執行程式。** Shadow DOM 只隔離一般 CSS；不是權限或執行隔離。
  它可以接觸 window／同 origin 內容；無限迴圈仍可能卡住整個頁面，workspace 不能強制回收其任意資源。
- CSP 限制 script／connect 同 origin，禁嵌框架；驗證 Host／Origin，拒絕跨站請求，無 CORS 開放。
  這些不等同登入，更不等同中央的管理授權。批准 plugin 的按鈕不是中央提權介面。
- 開關分頁保留各自狀態；關閉先 abort signal、執行已登記 cleanup，再呼叫 dispose。
  同步 lifecycle 失敗顯示於該面板；plugin 自行啟動的非同步工作仍須自行處理失敗與取消。
  動態 import 不能取消或真正卸載，已批准的 JS 可能在面板關閉後才完成 import；不再建立該面板。
- 工作區連線輪詢唯讀 catalog：斷線不銷毀既有面板，每 5 秒再試，單次 3 秒上限。Topic plugin 自己管理下方的診斷／預覽輪詢。

Settings live in vinc/config.hpp (browser settings in web/workspace/runtime.ts):

設定在 `vinc/config.hpp`（前端在 `web/workspace/runtime.ts`）：

| Setting | Default/behavior |
| --- | --- |
| ui_port | 8080 |
| ui_file_bytes / ui_snapshot_bytes | 8 MiB/file, 64 MiB startup bytes; indices/allocator/HTTP copies extra |
| ui_package_entries / ui_plugin_limit | 1024 entries/package,64 plugin directory entries |
| ui_manifest_bytes / ui_panels_per_plugin | 16 KiB /16 types |
| ui_identifier_bytes / ui_label_bytes | 80-byte IDs /128-byte manifest strings |
| ui_http_workers / ui_http_queue | 2 /16; HTTP closes excess connections |
| ui_http_timeout | 2 s read/write wait, not total request deadline |
| ui_start_timeout / ui_start_poll_interval | 5 s listener startup /5 ms observation |
| ui_stop_timeout | 10 s normal Module stop, then existing forced-exit policy |
| browser limits | 24 tabs,64 trust records,80-char title,240-char notice |

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

HTTP accepts GET/HEAD only, without bodies/upload/admin mutation. Slow/queued requests may extend worker join.2 s waits do not guarantee graceful exit within 10 s; existing Module termination policy bounds unresponsive shutdown.

HTTP 只接受 GET／HEAD，不接受 body，不提供 upload／修改管理 endpoint。慢客戶端及已排隊請求
可能延長 worker join；2 秒是等待 timeout，不承諾優雅停止必然在 10 秒內完成，必要時依 Module 政策強制退出。

<a id="plugin-開發"></a>

## Plugin development / Plugin 開發

See [PLUGIN.md](PLUGIN.md), source web/plugins/example/, SDK web/sdk/index.ts. Plugins need not use Vue and receive no Vue app/router/store.

見 [PLUGIN.md](PLUGIN.md)，最小範例原始碼位於 `web/plugins/example/`。
SDK 為 `web/sdk/index.ts`，不要求 plugin 使用 Vue，也不暴露 Vue app/router/store。

<a id="驗證"></a>

## Recorded validation / 驗證

```sh
cmake --build build/debug --target vinc_ui_tests vinc_ui_topic_tests vinc_ui_test_server vinc_ui_module_tests shell
ctest --test-dir build/debug -R "^ui\." --output-on-failure
cd src/vinc/ui/web
npm run typecheck
npx playwright install chromium
# VINC_UI_BUILD 指向欲測試的絕對 build 目錄；Windows 本機預設為 windows-msvc-debug。
npm test
```

Recorded 2026-10-14: Windows x64/MSVC 19.51/Node 24.19.0, ui.server/module/shell passed Debug/Release. Four Chromium scenarios/typecheck and desktop/mobile screenshots passed; Debug Module/Bus/Bag Shell regressions passed. APPS/EXAMPLES/PYTHON/TESTING-off SDK composition had no UI/Node/HTTP dependency. Later macOS UI build, type checking and manual HTTP results are in the [macOS report](../../../README.md#macos-validation); that report does not include browser automation, and the four-suite Linux WSL run does not cover UI.

本機 2026-10-14：Windows x64、MSVC 19.51、Node 24.19.0。
Debug／Release 的 `ui.server`、`ui.module`、`ui.shell` 均通過。
Chromium 四組瀏覽器案例與 Vue／TypeScript 型別檢查通過，檢視桌面與手機寬度截圖。
Debug 既有 `module.shell`、`message_bus.shell`、`bag.shell` 亦通過。
另以 APPS／EXAMPLES／PYTHON／TESTING 關閉的純 SDK 配置確認沒有 UI／Node／HTTP 相依。
後續 macOS 已完成 UI 建置、型別檢查與 HTTP 人工驗證，見[macOS 報告](../../../README.md#macos-validation)；該報告未含瀏覽器自動化，Linux WSL 四套回歸亦未涵蓋 UI。

<a id="待處理事項"></a>

## Outstanding work / 待處理事項

- **Confirmed next stage:** official management panels/central bridge must authorize official UI MID/Session, never name ui or self-declared official status. Ordinary Param permissions stay unchanged. Before write endpoints, design user login/central command authorization; read-only capability is not administrative authority.
- **Not implemented:** administrative streams, remaining feature plugins, relocatable installer, Electron; no placeholder RPC.
- **Validation limits:** native Linux/macOS, additional browsers, many/slow-client load. Listener failure has status/browser diagnostics but no automatic Module-fault supervision wiring.

- **已確認的下一階段**：官方管理面板與中央管理 bridge。須由中央核准官方 UI 的 MID／session，
  不能以 `ui` 名稱或 plugin 自稱官方取得權限；一般 Module 的 Param 權限維持現況。
  本輪只有防跨站 browser capability；未來加入修改 endpoint 前，仍須設計使用者登入及中央命令授權，不沿用唯讀 capability 冒充管理權限。
- **未實作**：修改管理資料串流、其餘官方 feature plugins、可重定位安裝包、Electron 包裝；不提供 placeholder RPC。
- **驗證限制**：原生 Linux／macOS、多瀏覽器及大量慢連線負載測試待執行。
  目前 HTTP thread 故障由 status／瀏覽器顯示，尚無自動轉換為 Module 故障的監督接線。

<a id="topic-monitor-契約2026-10-25"></a>

## Topic Monitor contract (2026-10-25) / Topic Monitor 契約（2026-10-25）

Start webUi, then open Topics→Topic Monitor. Lists registered topics, known type/hash, active Publisher/Subscriber counts, publication Hz, collisions, and Reader loss. Subscriber-only topics can appear; topics disappear under Bus rules when no registrations remain.

啟動 `webUi` 後，在左側 **Topics → Topic Monitor** 開啟。清單列出登記中的 Topic、
編譯時已知訊息型別／hash、活動 Publisher 與 Subscriber 數、發布 Hz、碰撞與讀者漏資料。
只有 Subscriber、尚無 Publisher 的 Topic 也可列出；沒有任何登記後依 Bus 規則移除。

Selecting a topic creates an ordinary Subscriber; get_last yields a validated local copy, formatted only after Reader lease release. It uses a Reader slot and can add contention: not a zero-cost passive probe. List sampling alone subscribes/copies no payload. No message history/injection/Param writes/Module controls.

選取 Topic 才建立正常 Subscriber，透過 `get_last()` 取得最新的已驗證本機副本，
在解除 Reader lease 後格式化。這不是被動零成本探針：會占一個 Reader 名額，也可能增加碰撞。
清單取樣不訂閱或複製 payload。沒有訊息歷史、注入訊息、修改 Param 或啟停 Module 功能。

<a id="採樣與燈號"></a>

### Sampling and indicators / 採樣與燈號

Manual main runs pumpOnce, diagnostics/preview work, then bounded sleep, without a new monitor worker. Context/Subscriber stay on main; HTTP workers read serialized caches and register bounded demands only. No cache lock is held during Context/mapping/copy/formatting.

Module 使用 Manual main：呼叫 pumpOnce、處理診斷與預覽，再有界睡眠；不新增監控 worker。
Context／Subscriber 只在主執行緒使用，HTTP workers 僅讀取已序列化快取及登記有界的預覽需求。
不持有快取 mutex 執行 Context、mapping、payload copy 或格式化。

| config | Default/meaning |
| --- | --- |
| ui_topic_sample_interval | 1 s sampling, not delivery deadline |
| ui_topic_stale_timeout | 3 s cache staleness makes data unavailable even with live HTTP |
| ui_topic_rate_window / ui_rate_samples | ~5 s /8 samples |
| ui_topic_recovery | 5 s without new issues; after wait ends start from first normal sample |
| ui_topic_red_wait | Current Writer wait>=1 s becomes red |
| ui_preview_interval / ui_monitor_poll_interval | 500 ms preview /20 ms idle |
| ui_preview_idle_timeout / ui_preview_topics | 5 s idle Reader release /8 previews |
| ui_monitor_topics / ui_topic_snapshot_bytes | 256 topics /4 MiB JSON; excess unavailable |
| ui_preview_message_bytes | 16 MiB complete image, checked before allocation; excess disables preview, not corruption |
| ui_preview_text_bytes / ui_preview_elements | 16 KiB text /32 array elements |
| ui_preview_nodes / ui_preview_depth / ui_preview_string_bytes | 256 nodes /16 levels /256 bytes per string |
| ui_session_token_bytes / ui_topic_name_bytes | 32 random bytes (16–64 allowed) /256 topic bytes |

| 設定（vinc/config.hpp） | 預設／意義 |
| --- | --- |
| ui_topic_sample_interval | 1 秒採樣；不是硬性送達期限 |
| ui_topic_stale_timeout | 清單或預覽快取超過 3 秒未更新即 unavailable；HTTP 仍有回覆也不保留舊綠燈 |
| ui_topic_rate_window / ui_rate_samples | 約 5 秒滾動窗口／最多 8 筆樣本 |
| ui_topic_recovery | 連續 5 秒沒有新問題；等待結束時由第一筆正常樣本開始觀察恢復 |
| ui_topic_red_wait | Writer 目前持續等待至少 1 秒顯示紅燈 |
| ui_preview_interval / ui_monitor_poll_interval | 預覽最多每 500ms 處理一次／main 閒置 20ms |
| ui_preview_idle_timeout / ui_preview_topics | 5 秒沒再請求就關閉 Reader／最多 8 個預覽 Topic |
| ui_monitor_topics / ui_topic_snapshot_bytes | 最多 256 Topic／序列化清單最多 4 MiB，超過回 unavailable |
| ui_preview_message_bytes | 完整映像最多 16 MiB，在接收配置前限制；超過只停用該預覽，不當成資料損壞 |
| ui_preview_text_bytes / ui_preview_elements | 整筆預覽最多 16 KiB 文字／每個陣列最多 32 元素 |
| ui_preview_nodes / ui_preview_depth / ui_preview_string_bytes | 最多 256 個走訪節點／16 層／每字串 256 bytes |
| ui_session_token_bytes / ui_topic_name_bytes | 預設 32 bytes OS 亂數（允許 16–64）／Topic 名稱最多 256 bytes |

The frontend reads actual policy thresholds. Hz derives from successful Publisher sequence deltas/steady intervals and sums current sources, not refresh/callback frequency. Initial sources show unmeasured; new IDs reset baselines, not negative rates. Scheduling/control latency makes the 5 s window approximate, without per-message timing/global atomic snapshots.

前端由回覆的 policy 取得實際門檻。瀏覽器刷新頻率、callback 次數不作 Hz 基準：
以各 Publisher 成功發布序號的差與 steady_clock 間隔計算，再加總目前來源。
初次樣本顯示尚未量測；新 Publisher ID 重建基準，不把重啟計數下降當成負流量。
取樣間有排程與控制請求延遲，約 5 秒窗口不是逐筆精確時間序列，也沒有全域原子快照。

- Green: no recent **observed** collision/loss/preview-read error, not general application health.
- Yellow: new collision/Reader loss/oversized skip, short wait, or recovery observation.
- Red: current Writer wait over threshold or explicit preview/read/registration error; show reason, never automatically kill a Module.
- Gray: browser/cache unavailable; mark stale rather than retain a misleading old green.

- 綠：近期沒有**觀察到**新碰撞／漏資料或預覽讀取錯誤；不是應用功能健康保證。
- 黃：觀察到新碰撞、Reader 漏資料／超量略過，或仍有短暫等待；恢復期間保持黃燈。
- 紅：Writer 當前等待達門檻，或預覽讀取／註冊有明確錯誤；顯示原因，不自動終止 Module。
- 灰：瀏覽器連線／診斷快取不可用。舊畫面標記 stale，不保留舊綠燈當成新狀態。

0 Hz can mean idle, not failure without an expected rate. If any active Publisher lacks a baseline, total is unmeasured, not zero. Historical collisions/max-wait do not permanently lock colors. Collision counts a Writer reservation waiting on Reader copy, not callback time. Lost sums confirmed skipped samples per Reader; one message missed by two Readers counts twice. Unpumped readers have not yet confirmed overwrites. Observed retired-source diagnostics remain during Topic lifetime, but counters are not transactional and exit races are best effort, not a lossless event log. Topic removal/central restart discards them.

0 Hz 可表示閒置，沒有期望頻率就不判故障。任一活動 Publisher 尚未有頻率基準時，Topic 合計先顯示尚未量測，不把未知當零。歷史碰撞／最長等待不永久鎖住燈色。
碰撞只計 Writer 一次 reservation 實際等待 Reader copy；與 callback 處理時間不同。
Lost 為各 Reader 確認跳過的筆數之和，同一筆被兩個 Reader 漏掉會計兩次。
沒有執行讀取／pump 的 Subscriber 還沒確認的覆蓋，不會預先出現在此計數。
關閉的 Reader／Publisher 已觀察診斷於 Topic 存續期間保留；計數跨欄位不是一致交易快照，
退出競爭邊界仍為盡力觀察，不是無損事件日誌。Topic 整體移除或中央重啟不保存這些診斷。

<a id="訊息預覽與-browser-capability"></a>

### Previews and browser capability / 訊息預覽與 browser capability

Existing visit_types/ribosome_visit_fields and designated Storage preview.hpp inspect compiled types, without dynamic loading/interpreters. Unknown schema keeps hash/stats and Unknown type preview. Arrays/nesting/strings are bounded with omissions and rendered via textContent. Strings are arbitrary bytes; non-ASCII/controls use explicit escapes, not guessed encodings. Pause retains text; hidden/closed tabs stop polling and idle TTL reclaims Readers. This is not total RSS budgeting: copies, metadata, JSON/HTTP/allocator add cost.

使用 Ribosome 已產生的 `visit_types`／`ribosome_visit_fields` 及 Storage 指定的
`detail/preview.hpp` 接線，沒有動態載入訊息或新增訊息直譯器。未知 schema 仍顯示統計／hash，
內容標示 Unknown type。大型陣列、巢狀資料與字串明確裁切；文字透過 DOM textContent 顯示。
字串本來是任意 bytes，非 ASCII／控制字元採明確 byte escape，不猜編碼。
暫停預覽保留目前文字；切到其他分頁／關閉分頁取消輪詢，Reader 於 idle timeout 後回收。
不承諾總 RSS 上限：接收副本、Storage metadata、JSON、HTTP 回覆與 allocator 另有成本。

GET /api/session requires X-Vinc-UI:1 and Host/Origin/Sec-Fetch-Site checks. OS randomness creates a service-lifetime token, held only in browser memory, never URL/cookie/storage. Topic/preview GETs require one Bearer Authorization. Restart invalidates it; clients may bootstrap again. No CORS/POST/cross-site preflight. The Topic API v1 serializes 64-bit IDs/hashes/counts/times as decimal strings. Graph capture time uses Unix milliseconds; graph IDs/hashes remain decimal strings.

`GET /api/session` 必須帶 `X-Vinc-UI: 1`，並通過 Host／Origin／Sec-Fetch-Site 檢查。
OS CSPRNG 產生本次 UI service 專用 token，存在 browser 記憶體，不放 URL、cookie 或持久儲存。
`GET /api/topics` 與 `/api/topics/preview?topic=...` 需單一 Authorization: Bearer token。
UI service 重啟即失效；前端可重新取得，HTTP 無 CORS／POST／跨站 preflight 開放。
資料 API 為 v1，64-bit ID、hash、計數與時間以十進位字串表示。

Capability blocks direct cross-site web reads, **not user login** or connected native clients. Trusted same-origin plugins already have page access and can fetch diagnostics; this is not a sandbox. Central uses the approved Context Session, never name ui or official claims, to supply ordinary Bus diagnostics.

此 capability 防跨站網頁直接讀取，**不是使用者登入**，也不抵抗有權連入的本機原生程式。
信任的同 origin UI plugin 本來就有完整頁面權限，亦能取得唯讀資料；不要把它當隔離 sandbox。
中央只依既有已核准 Context Session 提供一般 Bus 診斷，不因名稱 ui 或自稱官方而提升權限。

<a id="topic-monitor-實際驗證"></a>

### Topic Monitor validation record / Topic Monitor 實際驗證

Recorded 2026-10-25, Windows x64/MSVC 19.51.36257/CMake 4.3.2/Ninja/Node 24.19.0:

2026-10-25，Windows x64／MSVC 19.51.36257、CMake 4.3.2、Ninja、Node.js 24.19.0：

| Configuration | Related CTests | Repository-relative log |
| --- | --- | --- |
| Debug | 12/12,48.90 s | build/ui-topic-debug-tests.log |
| Release | 12/12,45.05 s | build/ui-topic-release-tests.log |

| 組態 | 相關 CTest | 紀錄（相對 repository root） |
| --- | --- | --- |
| Debug | 12/12，48.90 秒 | build/ui-topic-debug-tests.log |
| Release | 12/12，45.05 秒 | build/ui-topic-release-tests.log |

Scope: UI topics/server/module/shell, six Bus suites, Module context/process. Not all-project coverage; no Linux/macOS/MSVC 19.44 rerun. Related final builds had no warnings/errors and upstream sources were untouched.

範圍為 ui.topics／server／module／shell、六項 message_bus.* 及 module.context／process。
不是全專案全部 CTest，也未在 Linux／macOS 或 MSVC 19.44 重新驗證。
最終相關建置紀錄未見編譯警告／錯誤；未更動 upstream library。

- Real ui.module Host tests read UserExampleFrame, Publisher MID/name/Hz. A test Reader pins a lease: Writer wait>1 s red, release yellow, then 5 s green. Measured overwrites survive unsubscribe; unknown schema keeps diagnostics only.
- ui.topics checks 64-bit codecs, truncation, rejected versions/trailing bytes/cursors,32-Publisher pages below 16 KiB, warmup/restarts, wait/read-error recovery, nested previews/budgets. Controlled steady time tests cache expiry/Reader demand, not actual permanently stuck OS threads.
- Ring tests reject v2, preserve loss on unprotect, reset only quiescent slots; existing process/concurrent integrity/recovery passed.
- HTTP rejects missing token/header, foreign Origin, duplicate/missing/NUL queries; restart invalidates old tokens. HTTP workers never use Context.
- Typecheck and Chromium **5/5** passed: trust/lifecycle, colors, HTML-safe text, pause/resume, stale/disconnect gray, mobile width. Browser state fixtures are bounded simulations; separate CTests cover real process data paths.
- Actual Vinc+webUi+C++ example was browsed for camera/frames Hz/identity/nesting, then gray after central exit, with all children cleaned. Screenshot/log remain local build artifacts.

- `ui.module` 啟動真實 ModuleHost：讀取 UserExampleFrame、發布者 MID／名稱、實際發布 Hz；
  以既有測試 Reader 固定 lease，確認 Writer 超過 1 秒時紅燈、釋放後黃燈及 5 秒後恢復綠燈。
  慢 Reader 的覆蓋量在 HTTP 可見，退訂後保留；未知 schema 仍有診斷但不解碼。
- `ui.topics` 驗證 64-bit 數值 codec、各長度截斷、未知版本／尾端資料／游標拒絕、
  最大 32 Publisher 頁面仍小於 16 KiB、頻率暖機與重啟、等待／讀取錯誤恢復、
  巢狀陣列預覽及輸出預算。以可控制的 steady_clock 時間檢查清單／預覽過期與需求回收，
  沒有宣稱真的讓 OS／UI 執行緒永久掛住。
- 既有 RingBuffer 測試補入 v2 布局拒絕、unprotect 不清漏失計數、quiescent reset 才清計數。
  退出、保護複製及資料完整性的既有真實程序／並行案例通過。
- HTTP 驗證無 token、缺 bootstrap header、外來 Origin、重複／缺少／含 NUL query、
  UI 重啟使舊 token 失效。回覆快取不透過 HTTP worker 操作 Context。
- `npm run typecheck` 通過；Chromium **5/5** 通過，包含既有 plugin 信任／生命週期、
  Topic 燈號、預覽文字不執行 HTML、暫停／恢復、stale 與斷線轉灰、手機不溢出。
  瀏覽器狀態切換案例使用有界 API fixture；真實跨程序資料面由上述 CTest 另外驗證。
- 另以真正的 Vinc＋webUi＋既有 C++ userExampleModule，從 in-app browser 查看
  camera/frames 的即時 Hz、發布者與巢狀訊息；中央正常退出後畫面轉灰，所有測試程序已回收。
  本機畫面存於工作環境的 topic-monitor-live.png；生成截圖／log 不提交到原始碼。

That validation did not run cross-platform/multi-browser/high-topic-count/slow-HTTP stress. It added no forced restarts or changed collision/failure policy; the verification record is not a claim about current Git publication state.

上述驗證尚未涵蓋跨平台、多瀏覽器、大量 Topic／慢 HTTP 客戶端壓力。該次工作沒有新增強制重啟或改變碰撞等待／故障政策；這是歷史驗證紀錄，不描述目前 Git 提交／推送狀態。

<a id="module-graph"></a>

## Module Graph (2026-10-27) / Module 關係圖

Start `webUi`, then open **Module Graph → Module Graph**. The official `vinc.graph`
plugin draws `Module → Topic` for publication and `Topic → Module` for subscription.
It fetches once when opened and only on **Refresh** afterward. It adds no polling,
heartbeat thread, subscriptions, payload reads, or Hz sampling. Module borders show the sampled notification status described below.
The existing Topic Monitor retains its own sampling policy.

啟動 `webUi` 後開啟 **Module Graph → Module Graph**。發布為 `Module → Topic`，
訂閱為 `Topic → Module`；開啟時讀取一次，之後只在按 **Refresh** 時讀取。
不新增輪詢、heartbeat 執行緒、訂閱或訊息讀取，也不計算 Hz；Module 邊框呈現下述通知診斷。
既有 Topic Monitor 的取樣政策不變。

- MID identifies a Module; full topic name identifies a Topic. Ownership comes from
  authenticated Bus registrations, never a topic-name prefix. ModuleType is detail
  text, not an unstarted graph node. Isolated Modules and retained stopped records appear.
- Repeated registrations of one MID/topic/role share an edge with a count; both roles
  remain separate directed edges. Selection shows individual registration IDs/schema hashes.
- Drag, pan, zoom, search, selection/neighbour highlighting, Auto-layout and Fit view
  are browser-only. Refresh preserves surviving node positions and viewport; new nodes
  are placed to the right. Auto-layout reruns only when requested. Closing the panel
  discards its layout; no persistence is promised. Fill colours distinguish kinds; Module borders show sampled notification health as described below.
- Module pages and connection pages are sampled separately. The result is **not an
  atomic global snapshot**. A connection whose MID is absent becomes an unresolved node.
  Removed connections disappear after refresh; there is no historical dependency inference.
- A failed, timed-out, malformed or oversized refresh retains the previous graph with
  an error. A successful empty result clears it. No partial success is displayed.

- Module 以 MID、Topic 以完整名稱識別；所有權取自已驗證的 Bus 登記，不猜名稱前綴。
  顯示沒有 pub/sub 的 Module 及中央保留的停止紀錄，不顯示尚未啟動的 ModuleType。
- 同 MID／Topic／角色的重複登記合併成帶數量的邊，發布與訂閱方向分開。
  選取節點可查原始登記 ID、schema、MID、名稱、ModuleType 與文字狀態。
- 拖曳、縮放、平移、搜尋、鄰接標示、自動排版均只改瀏覽器畫面。刷新保留既有位置及視角，
  新節點放右側；重排需按 Auto-layout。關閉面板後不保留排版。
- 分頁之間不是全域原子快照；找不到 MID 時標示 unresolved，不推測名稱。
  失敗保留舊圖並顯示錯誤；成功空結果會清空；不顯示半份成功結果。

### Data path and limits / 資料路徑與上限

`GET /api/graph` uses the same origin/Host/Bearer capability checks as Topic Monitor.
HTTP workers submit to one `GraphRequests` mailbox. The existing UI Module main
executes `Context::list_modules` and `Context::list_topic_connections` using one shared
deadline. The existing Host watchdog is the sole reply receiver. Stop wakes waiting
HTTP workers before Server destruction; browser cancellation does not promise remote
cancellation of a request already admitted, which remains bounded by its deadline.
One refresh may run at a time; additional requests return 429. Timeout returns 504;
other unavailable/capacity/shutdown failures return 503. Neither response grants mutation rights.

`GET /api/graph` 沿用 Host／Origin／Bearer 檢查。HTTP worker 只投遞有界工作並等待，
既有 UI main 透過 Context 分頁取得資料，共用一個期限；Host watchdog 仍是唯一接收者。
停止時先喚醒 HTTP 等待者再回收 Server。瀏覽器取消不保證已接收的中央查詢立即取消，
仍受期限限制。同時只處理一筆刷新，其餘回 429；逾時 504；不可用／容量不足／停止中 503。

| Setting in `vinc::config` | Default / 預設 | Meaning / 用途 |
| --- | --- | --- |
| `ui_graph_timeout` | 10 s | Whole refresh deadline, including mailbox wait / 含等待佇列的整次期限 |
| `ui_graph_modules` | 256 | Maximum collected Module records / Module 紀錄上限 |
| `ui_graph_topics` | 1024 | Distinct topics / 不同 Topic 上限 |
| `ui_graph_connections` | 16384 | Raw registrations, before edge merging / 合併前登記上限 |
| `ui_graph_response_bytes` | 8 MiB | Complete JSON response / 完整 JSON 上限 |

The browser adds a 12 s fetch deadline, validates graph API v2 and the same record ceilings,
and retains 64-bit identities as decimal strings. Increasing server ceilings requires
updating the browser's protocol validation as well. These are safety ceilings, not a
performance claim for maximum-size layouts. Page-size constants remain in their owning
protocol codecs; the Control Channel payload ceiling is unchanged (16 KiB).

瀏覽器另有 12 秒 fetch 期限，驗證 graph API v2 及相同筆數上限；64-bit ID／schema 保持十進位字串。
調高 server 上限須同步調整 browser 驗證；最大布局效能尚未量測，不以容量上限宣稱流暢。
分頁筆數屬於各元件協定，Control Channel 的 16 KiB 上限不變。

Vue Flow 1.48.2 and Dagre 2.0.4 are bundled only into `vinc.graph`, with pinned npm
integrities and transitive licenses in `app/third-party.txt`. No CDN, browser Node runtime,
framework-wide frontend state or native dependency is introduced. Official trust hashes
the **built package bytes**, not the Vue/TypeScript sources.

Vue Flow／Dagre 只打包進 `vinc.graph`，不引入 CDN、瀏覽器 Node 或中央原生相依。
版本與 integrity 固定，第三方授權完整交付；官方信任雜湊以實際部署產物計算。

### Module Graph validation / 關係圖驗證

Windows x64 / MSVC 19.51 (toolset 14.51.36231): full Debug and Release builds
passed. The 42 selected Common error, Control Channel, Message Bus, Module System,
SDK and UI CTests passed in each configuration. They include real ModuleHost
pagination across nine isolated Modules and 43 registrations, subscriber-only topics,
authenticated MIDs, stopped-record retention and connection cleanup; codec truncation,
version/cursor rejection and 16 KiB limits; query timeout/late replies, pending-slot
recovery, Busy admission and shutdown wakeup. The SDK external-consumer test builds
with the matching MSVC developer environment.

TypeScript checking and all six Chromium workspace tests passed. The graph test
uses bounded HTTP fixtures to check initial fit after stylesheet/node measurement,
dragging, viewport/position preservation, manual-only fetches, selection/search,
layout controls, stale-graph retention after HTTP or malformed-timestamp failure,
empty results, close cleanup and mobile width. These browser fixtures complement
the real-process CTests; they are not a load benchmark. Screenshots/logs stay under
the local build directory.

Windows x64／MSVC 19.51：Debug、Release 完整建置通過，各 **42/42** 項相關 CTest 通過。
包含真實 ModuleHost 的跨頁清單（9 個孤立 Module、43 筆連線）、只有訂閱者的 Topic、
MID 所有權、停止後紀錄／連線回收，以及截斷／版本／游標／封包上限、逾時晚到回覆、
pending 額度回收、Busy 與停止喚醒。外部 SDK consumer 使用相符的 MSVC 開發環境建置。

TypeScript 檢查與 Chromium **6/6** 通過；圖形測試涵蓋首次量測縮放、拖曳、刷新保留位置／
視角、無自動查詢、選取搜尋、排版控制、HTTP／畸形時間戳失敗保留舊圖、空結果、關閉及手機寬度。
瀏覽器使用有界 HTTP fixture，真實跨程序路徑由 CTest 驗證；不宣稱是壓力測試。
截圖與 log 保留於本機 build，不提交生成檔。

Outstanding validation: very large graph layout responsiveness, sustained adversarial
refresh load, and native Linux/macOS remain unverified. Graph pages intentionally have
no global consistency token; refresh during registration/restart may show a transient
unresolved node until a subsequent refresh.

待驗證：大型圖排版效能、持續惡意刷新負載、Linux／macOS 實機。
分頁刻意不提供全域一致性 token；登記／重啟中的暫態 unresolved 可能到下次刷新才消失。

### Notification health on Module Graph / 關係圖通知健康狀態

Graph API v2 includes authenticated Module notification summaries. Existing manual refresh remains: no polling is added. Running/observed/no active fault is green, notification degradation yellow, lifecycle failure/restart waiting red, and inactive/unobserved/unresolved gray. Selection shows receiver/local sender/Publisher-route counters, first failure Unix ns, last affected MID and cause-aware error text. These are sampled reports, not heartbeat indicators or callback acknowledgements; historical errors remain after recovery. The complete contract is [Pump wakeup](../module_system/PUMP_WAKEUP.md).

Graph v2 加入 Module 通知診斷，仍由使用者手動刷新。綠／黃／紅／灰分別表示已觀察且運行無通知故障、通知降級、生命週期失敗／等待重啟、未活動／未觀察／未解析。選取後可看接收等待、本機送出與 Publisher 路由的計數及原因；歷史錯誤在恢復後保留，不宣稱為即時 heartbeat 或 callback 完成確認。

# Service network parameters / 服務網路參數

## Contract / 契約

| Parameter | Type | Registration default | Owner | Application point |
| --- | --- | --- | --- | --- |
| `vinc.shell_ip` | string | `127.0.0.1` | Central | Next App startup |
| `vinc.shell_port` | uint16 | `31410` | Central | Next App startup |
| `ui.port` | uint16 | `31411` | UI Module | Next HTTP service start |

All three use Limited permission. Shell settings declare `RestartHint::App`;
UI declares `RestartHint::Module`. Hints remain advisory metadata, not automatic
restart or pending-state tracking. Port **31412** is reserved in the project plan
for future Vinc-to-Vinc service only: no listener, Param or protocol is implemented.
Internal command/heartbeat communication continues using per-Module Local Socket
paths; it does not consume a TCP port. This project convention does not reserve
ports on the host or claim an IANA allocation.

三項均為 Limited；Shell 標記 App 重啟，UI 標記 Module 重啟。標記只提醒，
不自動重啟或追蹤待套用狀態。31412 僅記錄未來 Vinc-to-Vinc 用途，不開 listener、
不註冊 Param，也不宣稱已實作協定。內部 command／heartbeat 維持各 Module 的
Local Socket 路徑。這是專案預設值，不代表獨占主機埠或 IANA 配發。

Central loads persistence before registering Shell settings. The UI registers
its own `port` through ordinary `context.param` in `on_start()`, including when
HTTP is disabled, and uses `read_current()` before listening. Registration and
set acknowledge commit, not pump cache refresh. System Module ownership does not
bypass Param permissions. Other Modules cannot write these Limited settings.
`config.hpp` retains the registration defaults and standalone component defaults.

中央先載入檔案再註冊 Shell 設定；UI 即使停用 HTTP，也於 on_start 使用一般
Context 註冊自己的 port，並在監聽前用 read_current 取得副本。不得等待首次 pump
才取得啟動設定，也不得因 System Module 身分繞過權限。config 常數保留作首次
註冊及獨立元件預設。

## Overrides, validation and persistence / 覆寫、驗證與保存

Precedence is code default < saved Param < explicit startup argument. App
`--shell-ip`/`--shell-port` and UI's existing `--port` set the live value, never
the declaration default. They do not save implicitly; explicit `param save`
includes them. Reusing explicit arguments on a later start overrides the file
again. A default change follows ordinary Param registration reconciliation;
an existing user-customized saved value is not forcibly migrated to the new port.

優先順序為程式預設、已保存參數、本次明確啟動引數。CLI 改 live value，
不改註冊 default、不自動 save；手動 save 才保存。下次仍帶相同引數就再次覆寫。
新預設沿用 Param 的 default 對帳規則，不強制改掉使用者已自訂的舊埠。

Every consumed entry is copied and validated before adoption. Shell IP accepts
numeric IPv4/IPv6 only (no DNS). Ports are uint16; negative/out-of-range values
are rejected by typed Param parsing. Zero retains its existing meaning: let the
OS allocate a port. The configured value remains zero and the actual bound port
is reported separately.

Shell owns one `NetworkParams` consumer shared by local, remote and Tcl routers.
Startup and successful central `param set` commands for its two settings refresh
the consumer; there is no polling thread or listener mutation. An invalid IP
is restored to the last accepted IP, initially loopback, using ordinary set.
All current user-facing writers of these Limited central settings use these
routers; future framework writers must also invoke their owner's validation.
This is not a generic Param validator or a new validation hook for user Modules.

取得副本後先驗證；IP 只接受數字 IPv4／IPv6，不查 DNS。uint16 型別拒絕負數與
超界埠；0 仍表示 OS 選埠，Param 保持 0，實際埠另外顯示。Shell 在啟動及共用
router 的相關 set 命令後檢查，非法 IP 寫回最後接受值，起始為 loopback。
本機、遠端、Tcl 共用狀態，不新增輪詢執行緒，也不把業務檢查塞入 Param 核心。
日後新增中央寫入者，也須接上所屬消費端驗證。

Corrections neither save nor promise CAS: a later concurrent edit may race with
an ordinary correcting set. `param save` retains its existing behavior; it is
not a barrier waiting for Module callbacks. Invalid file input is corrected
in live state before binding, without silently rewriting that saved value.
Registration itself retains the existing default-reconciliation persistence rules.

修正不隱含 save、不承諾 CAS，並行寫入仍依既有責任邊界處理。save 不等待 Module
callback。檔案中的非法值先在 live 狀態修正再 bind，不悄悄保存修正值；註冊時的
default 對帳仍遵循既有保存規則。

## Listener lifetime and failures / 監聽生命週期與失敗

```text
param get vinc.shell_ip
param set vinc.shell_port 31420
param set ui.port 31421
param save
ui disable
ui enable
```

This explicitly saves both changes and restarts only HTTP. The Shell listener
continues on its existing endpoint until the App restarts. Use an available port
for the deployment; these example overrides are not additional project defaults.

此例明確保存兩項更改、只重啟 HTTP；Socket Shell 仍用舊端點直到 App 重啟。
範例埠是使用者覆寫，並非新增專案預設值，部署時須自行選擇可用埠。

Changing a parameter does not rebind Socket Shell or HTTP. Existing sessions
continue. Socket Shell consumes saved settings on the next App start. UI observes
Param through its ordinary pump callback, but updates only desired configuration.
`ui disable` then `ui enable` explicitly stops/restarts HTTP and reads the latest
committed port; the Module MID stays unchanged. `ui status` reports both `port`
(actual while listening) and `configured_port`. Enabling an already enabled UI
does not restart it. App/Module restart also consumes the latest applicable input.

改參數不換綁、不斷線。Shell 下次 App 啟動生效；UI callback 只更新期望設定。
明確 disable／enable 才停止並重新開 HTTP、重新讀取 port，MID 不變。
status 同時列出實際 port 與 configured_port。已啟用時再 enable 不算重啟。

A valid endpoint may still fail to bind because it is occupied, unavailable or
denied by the OS. Keep that requested value and report the failure; do not restore
defaults or silently search for another port. Existing policies remain: Socket
Shell startup failure aborts App startup; UI failure stays isolated and follows
the System Module retry policy. A usable fixed endpoint requires unique IP/port
configuration for concurrent Apps. Runtime settings remain per `INSTANCE_NAME`.

合法端點仍可能因占用、位址不存在或 OS 拒絕而 bind 失敗；保留指定值、回報錯誤，
不偷偷改回預設或換埠。維持原有政策：Socket Shell 啟動失敗中止 App 啟動；
UI 失敗隔離並遵循 System Module 重試政策。不同 App 的 Param 檔案雖隔離，
仍需各自安排不衝突的固定監聽端點。

## Validation / 驗證

Source validation on 2026-11-04 used Release builds:

| Environment | Related source tests |
| --- | --- |
| Windows 11 x64 build 26200, MSVC 19.51.36257, CMake 4.3.2, CPython 3.13.12 | 22/22 passed |
| WSL Ubuntu 24.04 x64, GCC 13.3, glibc 2.39, CMake 3.28.3, CPython 3.12.3 | 19/19 passed |

Coverage includes typed registration/permissions/restart hints, CLI/default/file
precedence, unsaved edits, invalid-file IP repair without saving, IPv6 and port 0,
local/remote/Tcl correction, connected-client continuity, UI disable/enable on a
new real port without changing MID, and occupied-port failure without rewriting
Param. Related Shell, Param, UI, central-entry/System-Module and Python-service
tests passed. Windows also exercised its three console tests. Full source builds
passed; the first Windows link attempt exposed a missing NetworkParams source in
the test-only Shell fault target, corrected before acceptance. Final review added
blank lines only; it did not change tested behavior.

本輪使用 Release：Windows 22/22、WSL 19/19。涵蓋上述優先序、保存、非法值、
IPv6／port 0、連線維持、UI 同 MID 換埠及埠衝突保留設定；也回歸 Shell、Param、
UI、中央入口／System Module、Python services。Windows 另含三項 console 測試。
完整來源建置通過；首次 Windows 連結發現測試專用 fault target 少列新來源，
補齊後完成驗收。最後審查僅補空行，不改測試行為。

Raw local evidence (ignored, not shipped): `build/service-ports/windows-ports.log`,
`linux-ports.log`, and their CTest XML reports. This is not a new full-project,
Debug or native macOS acceptance. Native macOS must rerun the same topic revision;
earlier Param or port-8080 results do not establish this migration's behavior.
The root TODO carries that follow-up.

本輪不是全專案／Debug／macOS 驗收；原始 log 與 XML 留在忽略的本機目錄。
macOS 必須驗同一分支，不能以舊 Param 或 8080 結果替代，已列根目錄 TODO。

Windows clean installed-SDK acceptance: **2/2 passed** (`sdk.installed_consumer`,
`sdk.installed_application`, 270.39 seconds). The test recreates the install prefix
and consumer using the tested Release producer; it is not a second clean producer
build. It verifies relocated SDK/App, CLI port 0 as live Param without changing
default 31410, UI Param/default 31411 and real HTTP, custom Bus/Bag/UI, incremental
schema/script rebuilding and orderly child exit. Log/XML:
`build/service-ports/windows-service-sdk.*`.

Windows 乾淨 SDK consumer／application **2/2 通過**，重新建立安裝前綴與 consumer，
沿用已測 Release producer，並非再做一次全新 producer。搬移部署後確認 Shell CLI 0／
預設 31410、UI 31411 及真實 HTTP、自訂 Bus／Bag／UI、增量重建與子程序退出。

WSL clean installed consumer/application also passed **2/2** (145.00 seconds),
with the same relocation, real HTTP and incremental-rebuild checks. Log/XML:
`build/service-ports/linux-service-sdk.*`. Final source audit compared 720
non-Markdown files with the WSL snapshot: no differences. Windows and WSL process
audits found no remaining test App/Module processes. Both SDK runs were serialized
to avoid interference at the default UI port. No system network settings changed.

WSL 乾淨 installed consumer／application 同樣 **2/2 通過**（145.00 秒），
包含搬移、真實 HTTP 與增量重建。最終核對 720 個非 Markdown 檔案均與 WSL 副本一致；
兩平台未遺留測試 App／Module。固定埠 SDK 驗收依序執行，避免互相占用，沒有更改系統網路設定。

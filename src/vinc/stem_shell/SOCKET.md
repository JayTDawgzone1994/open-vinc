# Socket Shell

Vinc 每次啟動都開啟 TCP Shell，預設 `127.0.0.1:5500`。沒有 Vinc 客戶端模式。
本層使用共用 CommandRouter、ScriptService、OutputHub 與 OSAL Socket／SocketPoller；
模組仍經既有控制通道進入 BaseModule hook，Bus 訊息內容不經 Shell。

## 啟動

```powershell
.\build\windows-msvc-debug\Vinc.exe
.\build\windows-msvc-debug\Vinc.exe --shell-ip 127.0.0.1 --shell-port 5501
.\build\windows-msvc-debug\Vinc.exe --shell-ip 0.0.0.0 --shell-port 5500 --shell-disconnect-timeout 30 -S examples/scripts/startup.tcl
```

| 參數 | 預設 | 說明 |
| --- | --- | --- |
| `--shell-ip` | `127.0.0.1` | 數字 IPv4／IPv6，例如 `::1`；不做 DNS 查詢 |
| `--shell-port` | `5500` | 0–65535；0 由 OS 配置，啟動時印出實際 port |
| `--shell-disconnect-timeout` | `30` | 斷線後保留 Session 的秒數，0–86400 |
| `--shell-send-timeout` | `30` | 有待送資料卻沒有送出進度的秒數，1–86400 |
| `--shell-max-connections` | `8` | 同時連線上限，1–8 |

框架參數須放在 `-S` 之前，腳本檔名之後全部交給 Tcl `argv`。
bind/listen 失敗時印出錯誤並結束啟動，部署腳本尚未執行；不偷偷換 port。
啟用服務且 stdin 非互動終端時，EOF 不會關閉框架；可經遠端 `exit` 完整退出。

第一版依已確認範圍不提供登入與加密。可連上者能操作模組、執行受信任 Tcl 腳本與關閉全框架。
非 loopback 綁定適用於受信任網路；密碼驗證為後續工作。

## PuTTY

- Connection type 選 **Raw**，Host `127.0.0.1`、Port `5500`（或自行設定的值）。
- Terminal → **Local echo: Force on**、**Local line editing: Force on**。
- 字元編碼使用 UTF-8，按 Enter 送出整行。

協定是 UTF-8 文字與 CR／LF／CRLF 行尾，回覆使用 CRLF；沒有 Telnet 協商、SSH、遠端編輯、歷史或 Tab 補全。
背景輸出可能打亂客戶端尚未送出的輸入列顯示，可用 `output off` 暫停觀察。
輸入列重繪仍是 [TODO](../../../TODO.md)。目前驗證 TCP 協定，尚未進行 PuTTY GUI 實測。

## 命令

```text
help
exampleModule
exampleNode hello
output off
output on
srun examples/scripts/maintenance.tcl
sstatus
scancel
disconnect
resume <恢復碼>
exit
```

- 每個連線最多一個一般命令工作；同連線重複送出回 Busy。網路執行緒不等待模組命令。
- 命令回覆只送請求者。模組 stdout／stderr 是 OutputHub 廣播，不能當作請求專屬回覆。
- 模組輸出預設開啟；`output on/off` 只影響本 Session，不影響命令回覆、腳本 `puts` 或其他 Session。
- `disconnect` 只關閉該連線，保留恢復期限；`exit` 等候全框架模組退出、Bus 清理與輸出收尾。
- 本機與遠端共用一個 Tcl 工作；第二次 `srun` 回 Busy。`sstatus` 查詢是否執行中；`scancel` 取消全域工作。
- 腳本輸出與結果只屬於啟動者。腳本失敗不自動關閉框架或停止既有模組。
- `output`、`disconnect`、`resume`、`sstatus` 是框架保留名稱，不允許模組占用。

## 斷線與恢復

歡迎訊息提供 256-bit 隨機恢復碼，由 OSAL 的 OS 隨機來源產生，不印到中央日誌。
斷線後腳本先繼續；期限內建立新連線並輸入 `resume <恢復碼>`，恢復原 Session 所有權。
成功後會發新碼，舊碼失效。已有連線的 Session、逾時 Session 及正在持有工作的申請者不能這樣轉移。
恢復碼是持有即可使用的憑證，不是登入機制，也不根據 IP 猜測使用者身份。

期限從 OS 回報斷線或框架判定傳送無進度時開始計算；不保證立即發現網路黑洞。
沒有待送資料的閒置連線不受 send timeout 影響。第一版沒有客戶端 heartbeat 協定。

期限過後僅要求取消該 Session 所有的 Tcl 工作，不取消別人的腳本、不撤銷命令、不停止已啟動模組。
阻塞原生程式碼仍可能延遲取消；不強殺中央執行緒。
斷線時未送完及斷線期間的輸出不保留，恢復時明確提示；命令不自動重送。
失去回覆時應查詢實際模組狀態，不能假定命令未執行。

## 容量與生命週期

- 每行最多 4096 bytes／256 參數，超限丟棄整行，不執行截斷前綴。拒絕無效 UTF-8、NUL 與控制字元（Tab 除外）。
- 每連線傳送佇列最多 1 MiB／128 筆，包含正在部分送出的項目；項目間不插入其他輸出。
  單次命令回覆／腳本批次最多 256 KiB，超限明確報錯；腳本暫存丟失會顯示標記。
- 傳送佇列壓力升高時暫停收命令；超出上限或 send timeout 時關閉該連線，進入斷線期限。
  命令回覆不採 OutputHub 的丟舊資料政策。
- 遠端 OutputHub 訂閱每個 128 KiB／64 事件，滿載丟最舊資料並報略過量；本機訂閱獨立。
- 一個網路執行緒配合有界命令工作。最多保留 `2 × max_connections` 個一般 Session（含斷線／尚未返回的工作），
  另有一個僅可恢復的入口位置。已逾時但尚未返回的原生工作仍占位置，避免反覆斷線製造無界工作。
- 全框架退出時停止接受連線／命令、取消腳本、等候模組及命令完成。
  最後網路輸出最多收尾 2 秒；慢客戶端不阻擋退出，未送完內容可能隨關閉丟失。

嵌入時先建構 `StemShell`、登記模組，再呼叫 `start_socket_shell(options)`。
建構嵌入式 Shell 本身不占 TCP port；Vinc 執行檔固定呼叫此入口。
`SocketShell` 借用的 ModuleManager 與 ScriptService 必須活得比它久；析構收回網路執行緒及命令工作。
`finish_shutdown()` 應在呼叫者已確認模組／Bus 清理完成後使用；析構本身不宣稱框架完整關閉。

## 驗證

Windows x64 MSVC Debug／Release 全部 23 個 CTest 套件通過。
新增真實 TCP 測試涵蓋分段與 CRLF 輸入、非法／超長命令、8 連線輸出訂閱及第 9 個拒絕、
IPv6、跨 Session 腳本互斥、恢復碼輪替、斷線取消歸屬、慢客戶端逾時、模組程序操作及退出尾端輸出。
另以 Vinc 執行檔驗證預設 port、非互動 stdin EOF、CLI 設定、port 衝突先於部署、錯誤 port 與遠端退出。
Linux／macOS 尚待實機建置與行為驗證；PuTTY GUI 尚未實測。

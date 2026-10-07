# 通用 OSAL Socket

Socket 是獨立的作業系統能力封裝，提供一致的位址、收發及就緒等待介面。
它不認識 Module、CMD、heartbeat、啟動握手、重連或 message bus。
本版本新增此介面，既有 Process／Pipe 尚未遷移或刪除。

## 介面與建置

| 標頭 | 公開能力 |
| --- | --- |
| `<vinc/osal/socket_address.hpp>` | IPv4／IPv6 位址、IP／本機 endpoint、同步 resolver |
| `<vinc/osal/socket.hpp>` | `Socket`、收發結果、型別安全選項、multicast |
| `<vinc/osal/socket_poller.hpp>` | `SocketPoller`、註冊 ID、level-triggered 就緒事件 |

連結 `vinc::osal`，使用 C++23。Windows 新增的 ws2_32 是 OS 原生庫，
沒有新增第三方套件。Winsock 初始化／清理由內部 runtime 管理，存活 socket
及 poller 持有 runtime，避免提前清理。公開標頭不包含 Windows 或 POSIX 型別。
CMake 選擇各平台 Poller 後端，私有 `socket_native.hpp` 封裝原生型別與呼叫差異。

| 能力 | Windows | Linux | macOS |
| --- | --- | --- | --- |
| IPv4／IPv6 TCP、UDP | Winsock | POSIX sockets | POSIX sockets |
| 本機串流 | AF_UNIX／SOCK_STREAM | AF_UNIX／SOCK_STREAM | AF_UNIX／SOCK_STREAM |
| 本機資料報 | 此版本回報 Unsupported | AF_UNIX／SOCK_DGRAM | AF_UNIX／SOCK_DGRAM |
| Poller | WSAPoll | epoll | kqueue |
| 跨執行緒喚醒 | 私有 loopback TCP pair | eventfd | EVFILT_USER |

Windows 的喚醒 pair 不承載應用資料；只在 Poller 建立時短暫監聽 loopback
隨機 port，核對接受端確實對應自己的 client，再關閉 listener。設定 TCP_NODELAY，
合併重複喚醒，最多保留一個尚未取走的通知。沒有背景執行緒。
這也表示 Windows Poller 需要系統允許 loopback TCP，不能宣稱完全不使用 IP 資源。

`SocketPoller::create(max_sockets, setup_deadline, token)` 限制喚醒 pair 的建立等待：
Windows 使用呼叫端期限與五秒的較早者。建立階段尚無喚醒通道時，可取消的短暫等待
以最多 10 ms 分段檢查 token；正常 `wait_until()` 使用事件喚醒，不定時輪詢。

這是通用功能基線，不宣稱封裝每種 OS 擴充：raw sockets、SEQPACKET、Linux abstract
namespace、ancillary data／fd 傳遞、原生 handle 匯入／匯出及 TLS 尚未提供。
不支援的組合不會暗中換成另一種傳輸。

## 位址

- `IPv4Address` 保存四個網路順序 bytes；`IPv6Address` 保存十六個 bytes 與數字 scope ID。
- `parse()` 只接受數字位址，不觸發 DNS。IPv6 scope 以第二個參數指定；
  `to_string()` 格式化位址本身，scope 保留在獨立欄位，不附加 `%zone`。
- `IpEndpoint::port` 使用 host byte order。IPv6 socket 預設 IPv6-only，
  可在 bind／connect 前明確設定 `socket_options::IPv6Only`，不依賴 OS 預設值。
- `LocalEndpoint::path` 是 UTF-8 pathname，長度按編碼後位元組檢查；NUL、無效 UTF-8
  及過長路徑回報 InvalidArgument。查詢未命名的本機對端可得到空 path，空 path 不可 bind。
- OSAL 不建立 runtime 目錄，不產生路徑，不自動移除既有名稱。
  `close()` 不刪除本機 socket 檔案；路徑擁有者須確認生命週期後明確移除。
  Windows 本機 socket 是特殊 reparse point，不宜用會追隨目標的 `filesystem::exists()`
  判斷；測試驗證目錄項目查詢與明確 `filesystem::remove()` 清理。
- `resolve_host(host, port, type, family)` 是同步 OS resolver，返回去重後的 IP endpoints。
  host 使用數字位址或 ASCII DNS／已轉成 ASCII 的 IDNA 名稱；不隱含 IDNA 轉換。
  此查詢不承諾 deadline／取消；呼叫端選擇位址、重試或連線競速策略。

## 非阻塞操作與等待

所有原生 socket 都保持 nonblocking。`try_*` 只嘗試一次操作，遇到 EINTR 的重試
不改成等待資料；目前無法進行時回報 WouldBlock。一般收發借用 `std::span`，
返回後 OSAL 不再保留 buffer，也不留下原生 overlapped I/O。

`*_until()` 在 WouldBlock 後等待就緒，再嘗試相同操作；需要等待才建立暫時 Poller。
它們沒有背景工作或內部傳送佇列，但建立暫時 Poller 會配置資源。大量連線或頻繁收發
應重用一個 `SocketPoller`，搭配 `try_*`，避免逐次建立等待資源。

- `accept_until()` 返回新 Socket；新端點同樣 nonblocking，並清除繼承旗標。
- `send_until()`／`receive_until()` 返回一次部分傳輸，不是 send-all／receive-exact。
- `send_to_until()`／`receive_from_until()` 對一個資料報操作，不拆成多個資料報。
- 已要求取消時不開始下一次操作；操作已產生結果時，返回該結果。
- 到期仍允許一次立即嘗試（poll 等待時間為零）；只有需要繼續等待時回報 Timeout。
  `SocketPoller::create()` 則在初始期限已到時直接拒絕建立。
- `vinc::Deadline` 由 common 的 `<vinc/time.hpp>` 提供，是 steady_clock 的絕對時間。取消或期限不是硬即時排程保證，
  原生系統呼叫、配置與資源清理亦不承諾固定完成時間。

## 串流與資料報

| 結果 | 契約 |
| --- | --- |
| 串流送出 N bytes | OS 已接受該部分；不代表對端程式已處理 |
| 串流收到 N bytes | N 可能小於 buffer，沒有訊息邊界 |
| 串流 `eof=true` | 對端寫入方向已結束，buffered data 已先讀完 |
| UDP 收到零 bytes | 有效空資料報，`eof=false` |
| 資料報 `truncated=true` | 返回前段資料與 copied count，尾端已丟棄 |
| WouldBlock | 本次未取得進展，不代表斷線 |

串流接收空 span 是 InvalidArgument，避免與 EOF 混淆；資料報允許空 span，
收到非空訊息時會明確回報截斷。資料報的原始完整長度不作跨平台保證。
`DatagramResult` 另含來源位址；本機資料報可能來自未命名端點。
`try_receive()` 也可用於已連接的資料報 socket，同樣保留 truncated／零長度語意。

所有傳輸結果都是本次操作結果，不保證請求唯一執行、可靠命令交付或自動重送。
POSIX 寫入斷線端點使用 MSG_NOSIGNAL／SO_NOSIGPIPE，避免終止宿主程序；不變更全域
signal handler。

## 建立連線

`try_connect()` 返回 Connected 或 InProgress。InProgress 代表原生連線正在進行；
註冊 Writable，收到事件後呼叫 `finish_connect()`，檢查 SO_ERROR 與對端位址。
可寫本身不代表連線成功。重複 `try_connect()` 不會更換正在連線的目的地，而回報 Busy。

POSIX 本機 socket 在 listener backlog 滿載時可能回報 EAGAIN，而沒有排入連線。
這種情況回報 WouldBlock，不冒充 InProgress；此版本 `connect_until()` 也會把該
錯誤交回並關閉 socket，由呼叫端選擇重試策略，避免在滿載 listener 上忙迴圈。

`finish_connect()` 保存已觀察到的成功或失敗結果。真正的連線失敗後，要建立新
Socket 再嘗試。UDP 的 connect 是設定對端關聯，不進行 TCP 式握手。

`connect_until()` 管理自己啟動的嘗試；失敗、逾時或取消會關閉該 socket。
對已連接的串流、監聽端或已由 `try_connect()` 啟動的嘗試呼叫此便利函式，
會拒絕操作且保留原 socket；手動啟動的連線請用 `finish_connect()` 完成。

`take_pending_error()` 會讀取並清除一般 socket 的 SO_ERROR；在連線進行中或已
記錄連線失敗時回報 Busy，避免診斷查詢吃掉 `finish_connect()` 應取得的結果。

## Poller 契約

`add(socket, Readable | Writable, user_tag)` 返回穩定、跨 Poller 不重用的註冊 ID
（限同一份已載入 OSAL）。重複註冊同一 socket 回報 Busy。預設上限 1024，可在建立時指定；
額滿回報 ResourceExhausted。註冊上限及輸出 span 限制內部儲存，不無界排隊。

`modify()` 改變興趣事件。原生更新失敗時，移除該註冊，避免只更新一個方向；
參數檢查失敗不改變既有註冊。`remove()` 解除監看，不關閉 socket。

`wait_until()` 是 level-triggered readiness，並非 I/O completion：

- 返回一筆或多筆帶有 registration、user_tag 的事件。相同註冊可能有分開的讀／寫事件。
- 返回零代表明確 wake；期限到期回報 Timeout，stop token 回報 Cancelled。
- 取消只停止此次等待，不關閉 socket，也不取消呼叫端自己開始的 connect。
- Error／Hangup 是提示；要透過收發或 finish_connect 取得最終結果。
  事件可能同時表示有資料與對端關閉，不能看到 Hangup 就丟掉尚未讀取的資料。
- 跨平台不保證每種半關閉都收到相同 flags。關注 Readable，實際接收到 EOF 才是
  接收方向結束的依據；就緒與後續操作之間仍可能出現 WouldBlock。
- 寫入資料送完後應取消 Writable 興趣，避免一個持續可寫的 socket 讓事件迴圈空轉。
- 不接管 socket 所有權；正常流程先 remove，再 close。誤先關閉／析構的舊註冊會
  在下次操作時清除；透過原生 handle 重用不能把舊事件指向新 socket。
- 一個 Poller 同時只允許一個 waiter；add／modify／remove／移動／析構不得與 wait
  並行。唯一可從其他執行緒直接呼叫的是 wake；Poller 物件生命週期仍須同步。

Windows WSAPoll 掃描全部註冊，成本是 O(N)；小輸出陣列採輪替順序避免尾端飢餓。
這是明確的 readiness 後端選擇，不宣稱等同 IOCP 大規模完成通知的效能。
三平台尚未有吞吐或尾端延遲 benchmark。

## 選項、所有權及錯誤

`set_option<socket_options::NoDelay>(true)` 與 `get_option<...>()` 提供型別安全選項。
TCP NoDelay／KeepAlive、buffer 大小、IPv6Only、UDP Broadcast、multicast loop/hops
都會檢查 family／type。另提供 IPv4 位址／IPv6 interface index 的 multicast
interface 查詢設定、加入與離開群組。

ReuseAddress 僅承諾 POSIX SO_REUSEADDR，Windows 回報 Unsupported；
ExclusiveAddressUse 是 Windows 選項，POSIX 回報 Unsupported。
這些選項和 IPv6Only 必須在 bind／connect 前設定，不把同名常數當成相同行為。
buffer 設定可能被 OS 調整；get 返回 OS 實際值，不保證與要求值相等。

Socket move-only；SocketPoller 也 move-only。`close()` 可重複呼叫，會讓此物件失效，
即使回報原生 close 錯誤也不對舊數字 handle 重試。`shutdown()` 只改變方向，並不
釋放 socket；不暗中 flush，也不承諾對端已收到或處理所有資料。

一條 socket 可由兩個執行緒分別讀／寫；同方向操作由呼叫端序列化。bind、connect、
options、close、移動與析構等狀態變更需與其他存取同步。上層安排自己的 event loop，
OSAL 不執行 callback、業務工作、程序建立、驗證或重連政策。

OS 失敗使用 `vinc::Result`／`vinc::Error`，保留統一分類、操作及原生來源／數值。
最外層 operation 對應呼叫的 Socket API；例如 receive_until 的等待逾時為
ReceiveSocket ← WaitSocket，code 仍為 Timeout。Poller 初始化或登記失敗亦保留實際原因，
同一操作的直接轉交不重複包裝；完整規則見 [OSAL 錯誤契約](README.md#統一錯誤介面)。
POSIX resolver 使用獨立原生來源 PosixResolver，不與 errno 混淆。
物件建立、地址字串與 resolver 可拋出標準配置例外；不承諾全 API noexcept。
IP 的 try_send／try_receive 不逐次配置 buffer；本機來源地址結果可能配置字串。

描述元預設 close-on-exec／不可繼承。macOS 等無原子建立旗標的路徑存在設定旗標前
的短暫區間；宿主自行使用 foreign fork／spawn 時也必須管理繼承，不能把這個 API
當成跨程序安全沙箱或任意宿主 fork 的同步機制。

## 驗證

```sh
cmake -S src/vinc/osal -B build/osal -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/osal
ctest --test-dir build/osal --output-on-failure
```

`osal.socket` 使用實際 IPv4／IPv6 TCP、UDP、本機 socket 及獨立子程序，涵蓋：
數字地址與 scope、resolver、部分收發、EOF／半關閉、資料報空訊息／截斷、
手動非阻塞連線、拒絕連線、期限／取消、背壓、就緒公平性、喚醒、stale ID、
UTF-8 本機路徑、路徑清理、對端被終止，以及反覆啟停的資源回收。
Multicast 驗證選項與 IPv4 loopback 介面的加入／離開，不代表實際網路上的群播交付驗證。
測試使用短的獨立本機 socket 路徑；過長的建置工作目錄可能超過 AF_UNIX 限制。

Windows 實測環境：OS build 26200、MSVC 19.44、SDK 10.0.26100、CMake 3.31.6、
Ninja 1.12.1。Linux／macOS 後端尚未原生建置與執行；Windows 最低支援版本也仍需
另行驗證，不能把在本機通過視為所有版本皆通過。TCP／UDP 需要本機 loopback 可用，
測試不依賴外部網路服務。

原生契約參考：

- [Windows WSAPoll](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsapoll)
- [Windows AF_UNIX](https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/)
- [非阻塞 connect 與 SO_ERROR](https://man7.org/linux/man-pages/man2/connect.2.html)
- [Windows 資料報截斷](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-recvfrom)
- [地址重用與獨占綁定](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse)

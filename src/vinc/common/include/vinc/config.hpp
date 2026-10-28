#pragma once

// Temporary home for centralized VINC system settings.
// Runtime defaults will later be managed by Param; layout settings stay compile-time.
// 系統設定暫存區：執行期預設未來交由 Param；影響型別布局的設定仍維持編譯期。
// 下方數值目前皆為編譯期常數；註解中的 KiB／MiB／GiB 分別代表 1024／1024²／1024³ bytes。
// 容量與間隔不等於硬即時保證；可覆寫的預設值及硬上限依各元件契約區分。
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace vinc::config {
// 【RPC】服務容量、呼叫排隊與期限。
// RPC admission, storage and scheduling bounds; see rpc/README.md.
// 每個 RPC 槽位的 request 完整訊息映像預設容量（bytes，含 metadata）。
inline constexpr std::uint64_t rpc_request_bytes = 64 * 1024;
// 每個 RPC 槽位的 response 完整訊息映像預設容量（bytes，含 metadata）。
inline constexpr std::uint64_t rpc_response_bytes = 64 * 1024;
// 每個 RPC 服務預設配置的共享槽位數；每格各有 request 與 response 區。
inline constexpr std::uint32_t rpc_slots = 8;
// 單一 RPC 服務允許配置的共享槽位數硬上限。
inline constexpr std::uint32_t rpc_slot_limit = 256;
// 同一中央所有 RPC 服務共享池的合計容量上限（bytes）。
inline constexpr std::uint64_t rpc_shared_bytes = 256ULL * 1024 * 1024;
// 每個 Context 保留的 RPC request 本地快照合計容量上限（bytes）。
inline constexpr std::uint64_t rpc_local_bytes = 64ULL * 1024 * 1024;
// 每個 Context 尚未回收的 RPC 呼叫數上限，含已逾時但仍須保留簿記的呼叫。
inline constexpr std::size_t rpc_pending_limit = 64;
// 同一中央的 RPC 服務數上限，含已退出但仍等待 lease 回收的服務。
inline constexpr std::size_t rpc_service_limit = 256;
// 中央 inbox、Host 與各 peer 控制佇列各自容納的 RPC 控制事件數上限。
inline constexpr std::size_t rpc_control_limit = 4096;
// Leave control-queue headroom for grants, completions, releases and registration.
// 同一中央已借出的 RPC 槽位總數上限；保留控制佇列空間供完成、釋放與登記使用。
inline constexpr std::size_t rpc_active_limit = 1024;
// RPC 服務名稱長度上限（bytes）；名稱另受 ASCII 命名規則限制。
inline constexpr std::size_t rpc_name_bytes = 128;
// RPC 應用回覆的 reason 文字長度上限（bytes）。
inline constexpr std::size_t rpc_reason_bytes = 256;
// RPC 呼叫的預設等待時間（秒），亦供服務登記等待使用；呼叫逾時由 pump 檢查。
inline constexpr auto rpc_call_timeout = std::chrono::seconds(5);
// 呼叫端可指定的 RPC 等待時間上限（小時）；到期不代表對端沒有執行。
inline constexpr auto rpc_max_call_timeout = std::chrono::hours(24);
// 中央 RPC worker 排送重試與退出補查間隔（毫秒），不是 handler 的執行期限。
inline constexpr auto rpc_control_retry = std::chrono::milliseconds(5);

// 【Param】參數命名、共享快照與檔案保存；這裡的既有設定尚未搬入 Param。
// Param admission and persistence bounds; existing framework settings are not migrated to Param.
// 參數完整名稱長度上限（bytes），包含 module 名稱與中間的點。
inline constexpr std::size_t param_name_bytes = 256;
// 單一字串型參數的內容長度上限（bytes），不是 Unicode 字元數。
inline constexpr std::size_t param_string_bytes = 256;
// 中央參數條目數，以及每個 Context 監聽參數數量的上限。
inline constexpr std::size_t param_entry_limit = 1024;
// 每個 Host 同時保留的 Param 未完成請求數上限。
inline constexpr std::size_t param_pending_limit = 32;
// 中央 Param 服務待處理工作佇列的筆數上限。
inline constexpr std::size_t param_queue_limit = 256;
// A／B 每個槽位的參數快照容量（bytes）；共兩槽，另有共享控制標頭。
inline constexpr std::size_t param_slot_bytes = 1024 * 1024;
// Param TOML 檔案讀取與寫出的大小上限（bytes）。
inline constexpr std::size_t param_file_bytes = 4 * 1024 * 1024;
// 中央命令等待 Param 回覆時檢查取消要求的間隔（毫秒），不是參數更新週期。
inline constexpr auto param_wait_poll_interval = std::chrono::milliseconds(10); // Active caller cancellation checks.
// Param 控制請求等待回覆的期限（秒）。
inline constexpr auto param_request_timeout = std::chrono::seconds(10);
// 讀寫參數共享快照時等待跨程序鎖的期限（秒），等待可取消。
inline constexpr auto param_lock_timeout = std::chrono::seconds(2);
// 【中央身分與資源紀錄】限制啟動保留名稱及崩潰後的清理工作量。
// Bounds for central admission and its process-crash resource journal.
// 中央建立新的隨機 epoch 名稱前綴時，允許嘗試保留名稱的次數。
inline constexpr std::size_t instance_name_attempts = 32;
// Retained prefix reservations bound disk metadata and startup scanning.
// 中央啟動時掃描舊 epoch 保留目錄的數量門檻；用來限制磁碟簿記與掃描工作量。
inline constexpr std::size_t instance_epoch_limit = 65536;
// 每個中央 epoch 的資源記錄數上限；共享記憶體與通知資源共用此額度。
inline constexpr std::size_t instance_resource_limit = 16384;
// 中央可登記供 Module 使用的共享鎖數上限，亦決定固定鎖表的容量。
inline constexpr std::size_t module_lock_limit = 64;
// 每個 Module 同時保留的未完成共享鎖控制請求數上限。
inline constexpr std::size_t module_lock_pending_limit = 32;
// 【共享鎖與 Module】重試、pump 補查、通知健康與本地執行監督。
// Shared mutex polling sleeps locally; cancellation wakes the sleep. This is a
// retry policy, not an OS scheduling or hard completion guarantee.
// 共享 mutex 競爭時的本地重試休眠間隔（毫秒），可被取消喚醒；不是持鎖期限。
inline constexpr auto concurrency_retry_interval = std::chrono::milliseconds(5);
// Idle data polling and Host supervision of a locally completed/failed main.
// pumpLoop 的閒置補查等待間隔（毫秒）；正常通知可提前喚醒，不限制 pumpOnce。
inline constexpr std::chrono::milliseconds module_pump_idle_interval{10};
// Failed notification handles are reopened by existing runtime progress, in milliseconds.
// 通知 handle 故障後，既有 runtime 流程重新開啟／重試的退避間隔（毫秒）。
inline constexpr std::chrono::milliseconds notification_retry_interval{1000};
// One initial failed send and one ordinary retry precede low-frequency backoff.
// 通知連續發送失敗幾次後進入低頻退避；含首次失敗與一般重試。
inline constexpr std::uint32_t notification_send_failures_before_backoff = 2;
// Repeated diagnostics are coalesced; state transitions may report immediately.
// 重複通知故障診斷的合併回報間隔（毫秒）；健康狀態轉換可立即回報。
inline constexpr std::chrono::milliseconds notification_report_interval{1000};
// Host 監督本地 main 完成／失敗與停止進度時的補查間隔（毫秒）。
inline constexpr std::chrono::milliseconds module_runtime_poll_interval{20};
// 每個 Context 可保留的 callback 訂閱項目數上限，不是每輪 callback 執行次數。
inline constexpr std::size_t module_callback_limit = 256;


// 【致命錯誤監督】中央管理出錯 Module 的退出寬限。
// Central's grace after first observing a fatal report. Earlier startup or
// explicit-stop deadlines win; expiry requests OS termination, not proof of exit.
// 中央首次觀察到 fatal 後允許退出的寬限（秒）；較早期限優先，到期要求 OS 終止但不保證已退出。
inline constexpr auto module_fatal_exit_timeout = std::chrono::seconds(5);
static_assert(module_fatal_exit_timeout > std::chrono::seconds::zero());

// 【錯誤原因鏈】這是型別布局設定，不是可動態調整的執行參數。
// Total stored error records, including the current error. This changes C++ layout;
// every linked consumer must use the same value and be rebuilt after a change.
// Unlike runtime settings, this capacity cannot be changed by Param.
// Error 原因鏈儲存的總層數（含目前錯誤，最少兩層）；影響布局，所有使用端須同值重建，不可改成執行期 Param。
inline constexpr std::size_t error_chain_max_depth = 3;
static_assert(error_chain_max_depth >= 2, "Error chains require at least two records");

// 【Message Bus】發布槽位、參與者、共享容量與控制請求。
// Message Bus defaults. Byte budgets include the complete encoded message image.
// 每個 Publisher 的 RingBuffer 預設槽位數；Bus 最低保留兩格。
inline constexpr std::uint32_t message_bus_default_slots = 2;
// 每個 RingBuffer 槽位的完整訊息映像預設容量（bytes，含 metadata）。
inline constexpr std::uint64_t message_bus_default_message_bytes = 1024 * 1024;
// Bus 預設允許的不同 Topic 數量上限。
inline constexpr std::uint32_t message_bus_default_topics = 128;
// 每個 Topic 預設可登記的 Publisher 數量上限。
inline constexpr std::uint32_t message_bus_default_publishers_per_topic = 32;
// 每個 Topic 預設可登記的 Subscriber 數量上限。
inline constexpr std::uint32_t message_bus_default_subscribers_per_topic = 128;
// 同一 Bus 的共享記憶體配置總預算預設值（bytes）。
inline constexpr std::uint64_t message_bus_default_shared_bytes = 1024ULL * 1024 * 1024;

// Control backpressure and the deadline for an unanswered registration request.
// Bus 控制端保留的未完成請求數上限，用於限制登記等控制操作的背壓。
inline constexpr std::size_t message_bus_pending_requests = 32;
// BusManager 控制工作佇列的筆數上限；不存放訊息 payload。
inline constexpr std::size_t message_bus_manager_queue_size = 256;
// Bus 登記等控制請求等待回覆的期限（秒），不是 Reader 競爭等待期限。
inline constexpr auto message_bus_request_timeout = std::chrono::seconds(10);

// Retry policy only: protected reader copies still have no contention timeout.
// The pre-increment comparison yields for attempts 1..63, then sleeps.
// 等待 Reader 釋放保護時切換到休眠的重試門檻；前 1～63 次 yield，之後才 sleep。
inline constexpr std::uint32_t message_bus_reader_sleep_threshold = 64;
// 進入 Reader 競爭休眠後的重試間隔（毫秒）；不替無限等待增加逾時。
inline constexpr auto message_bus_reader_retry_interval = std::chrono::milliseconds(1);

// 【OutputHub】原始 stdout／stderr 擷取、接收佇列與文字顯示。
// OutputHub queue and capture defaults. Byte limits count raw payload bytes.
// 每個 OutputHub 接收佇列的原始資料預設容量（bytes）。
inline constexpr std::size_t output_hub_default_queue_bytes = 256 * 1024;
// 每個 OutputHub 接收佇列的預設事件數上限，亦限制 metadata 筆數。
inline constexpr std::size_t output_hub_default_queue_events = 128;
// OutputHub 預設可同時擷取的程序數，包含仍在收尾的舊世代。
inline constexpr std::size_t output_hub_default_captures = 128;
// OutputHub 預設可登記的輸出接收者數上限。
inline constexpr std::size_t output_hub_default_subscribers = 8;
// 所有 OutputHub 接收佇列容量的預設合計預算（bytes）。
inline constexpr std::size_t output_hub_default_total_queue_bytes = 2 * 1024 * 1024;
// 所有 OutputHub 接收佇列容量的預設合計事件數預算。
inline constexpr std::size_t output_hub_default_total_queue_events = 1024;
// 程序退出後繼續讀取 stdout／stderr 尾端的預設寬限（毫秒）。
inline constexpr auto output_hub_default_drain_timeout = std::chrono::milliseconds(2000);
// 接收者一次 take 預設最多取回的輸出事件數。
inline constexpr std::size_t output_hub_default_take_events = 32;

// Admission limits and worker budgets. Shutdown and tail drain are separate deadlines.
// OutputHub 允許設定的同時擷取程序數硬上限。
inline constexpr std::size_t output_hub_max_captures = 1024;
// OutputHub 允許設定的接收者數量硬上限。
inline constexpr std::size_t output_hub_max_subscribers = 64;
// 輸出來源名稱長度上限（bytes），用來限制來源 metadata。
inline constexpr std::size_t output_hub_max_source_name_bytes = 256;
// 允許設定的程序退出後尾端擷取寬限上限（秒），與 Hub 自身關閉期限不同。
inline constexpr auto output_hub_max_drain_timeout = std::chrono::seconds(60);
// 每條 stdout／stderr 串流一次 Pipe 讀取的區塊容量（bytes）。
inline constexpr std::size_t output_hub_read_chunk_bytes = 4096;
// 收集執行緒一批最多處理的 Pipe I/O 完成事件數。
inline constexpr std::size_t output_hub_completion_batch_size = 32;
// OutputHub 收集流程等待／補查控制狀態的間隔（毫秒）。
inline constexpr auto output_hub_poll_interval = std::chrono::milliseconds(20);
// OutputHub 遇到持續失敗後的重試退避（毫秒），避免忙迴圈。
inline constexpr auto output_hub_failure_backoff = std::chrono::milliseconds(20);
// OutputHub 自身關閉時的收尾等待期限（秒），不是整個框架的退出期限。
inline constexpr auto output_hub_shutdown_timeout = std::chrono::seconds(2);

// Text presentation limits are independent of the raw Pipe read chunk size.
// 文字呈現器同時保留未完成行狀態的來源數上限。
inline constexpr std::size_t output_hub_text_max_sources = 256;
// 文字呈現器單段行內容上限（bytes），超長輸出會分段顯示。
inline constexpr std::size_t output_hub_text_line_bytes = 4096;
// 未換行文字等待顯示的間隔（毫秒），避免片段一直留在緩衝區。
inline constexpr auto output_hub_text_flush_interval = std::chrono::milliseconds(100);

// 【本地 Shell】輸入、命令、背景工作與完整關閉流程。
// Local Shell admission limits; byte limits apply before parsing or editing.
// 本地 Shell 命令列輸入的長度上限（bytes），在解析／編輯前限制。
inline constexpr std::size_t shell_max_line_bytes = 4096;
// 單一 Shell 命令解析後允許的參數數量上限。
inline constexpr std::size_t shell_max_arguments = 256;
// 本地 Shell 保留的命令歷史筆數上限。
inline constexpr std::size_t shell_history_entries = 128;
// Shell 可同時維護的非同步工作數上限。
inline constexpr std::size_t shell_max_jobs = 16;

// Keep output batches bounded so command results and keyboard input get a turn.
// 本地 Shell 一輪最多取出的背景輸出事件數，保留鍵盤與命令處理機會。
inline constexpr std::size_t shell_output_batch_events = 16;
// 本地 Shell 最後收尾階段一批最多取出的背景輸出事件數。
inline constexpr std::size_t shell_final_output_batch_events = 128;
// Shell 執行 Module 命令時等待回覆的預設期限（秒）。
inline constexpr auto shell_command_timeout = std::chrono::seconds(5);
// 本地 Shell 查詢自動完成候選的等待期限（秒）；遠端不提供自動完成。
inline constexpr auto shell_completion_timeout = std::chrono::seconds(1);
// 本地終端輸入與控制狀態的等待／補查間隔（毫秒）。
inline constexpr auto shell_terminal_poll_interval = std::chrono::milliseconds(20);
// 無互動終端時的 Shell 退避間隔（毫秒），避免沒有輸入仍空轉。
inline constexpr auto shell_headless_backoff = std::chrono::milliseconds(20);

// Shutdown has no overall timeout. These intervals control collection and diagnostics.
// Shell 完整關閉時收集程序退出狀態的補查間隔（毫秒）；沒有整體關閉逾時。
inline constexpr auto shell_shutdown_poll_interval = std::chrono::milliseconds(250);
// 完整關閉仍在等待時，顯示進度診斷的間隔（秒）。
inline constexpr auto shell_shutdown_report_interval = std::chrono::seconds(1);
// Shell 查詢背景工作完成狀態的間隔（毫秒）。
inline constexpr auto shell_job_poll_interval = std::chrono::milliseconds(10);

// 【Socket Shell】遠端連線、斷線保留、回覆佇列與 I/O 處理預算。
// Remote Shell defaults. Timeouts measure disconnected ownership and stalled sends,
// not the lifetime of an idle, healthy TCP connection.
// Socket Shell 預設綁定位址；只監聽本機 loopback，非遠端 Vinc 連線目標。
inline constexpr char shell_socket_default_ip[] = "127.0.0.1";
// Socket Shell 預設 TCP 監聽埠；可由啟動設定覆寫。
inline constexpr std::uint16_t shell_socket_default_port = 5500;
// 遠端斷線後保留 Session／腳本擁有者的寬限（毫秒），不是健康閒置連線壽命。
inline constexpr auto shell_socket_disconnect_timeout = std::chrono::milliseconds(30000);
// 尚有待送資料卻無傳送進度時的等待期限（毫秒），不是整條連線的總期限。
inline constexpr auto shell_socket_send_timeout = std::chrono::milliseconds(30000);
// Socket Shell 同時接受的正常客戶端連線數上限。
inline constexpr std::size_t shell_socket_max_connections = 8;
// One listener plus live clients. Retained sessions also cover reconnecting owners;
// an extra provisional session can restore an owner when ordinary storage is full.
// 遠端 Shell poller 的容量：所有客戶端加上一個 listener。
inline constexpr std::size_t shell_socket_poller_capacity = shell_socket_max_connections + 1;
// 保留 Session 容量相對於正常連線上限的倍數，包含斷線等待重連的擁有者。
inline constexpr std::size_t shell_socket_retained_session_factor = 2;
// 正常 Session 容量已滿時，供既有擁有者恢復連線的額外暫時名額。
inline constexpr std::size_t shell_socket_resume_slots = 1;
// 腳本工作信箱總容量：遠端保留 Session 加上一個本地擁有者名額。
inline constexpr std::size_t shell_script_mailboxes =
    shell_socket_max_connections * shell_socket_retained_session_factor + 1; // Local owner included.

// Network queue bytes count escaped wire text; the payload limit applies before escaping.
// 每個遠端 Session 的待送文字佇列總容量（bytes，依轉義後 wire 文字計算）。
inline constexpr std::size_t shell_socket_queue_bytes = 1024 * 1024;
// 單筆遠端回覆在轉義前的 payload 長度上限（bytes）。
inline constexpr std::size_t shell_socket_payload_bytes = 256 * 1024;
// 遠端待送佇列為命令／必要回覆保留的空間（bytes），背景收集不能吃滿此額度。
inline constexpr std::size_t shell_socket_reply_reserve_bytes = 600 * 1024;
// 每個遠端 Session 待送回覆佇列的項目數上限。
inline constexpr std::size_t shell_socket_queue_items = 128;
// 待送項目達到此門檻就暫緩收集背景輸出，保留回覆容量。
inline constexpr std::size_t shell_socket_collect_threshold_items = 120;
// 每個遠端 Session 的 OutputHub 原始背景輸出佇列容量（bytes）。
inline constexpr std::size_t shell_socket_output_queue_bytes = 128 * 1024;
// 每個遠端 Session 的 OutputHub 背景輸出佇列事件數上限。
inline constexpr std::size_t shell_socket_output_queue_events = 64;
// 遠端 Session 每輪最多從 OutputHub 取出的背景輸出事件數。
inline constexpr std::size_t shell_socket_output_batch_events = 4;
// listener 一輪最多接受的新連線數，避免連線洪流獨占工作迴圈。
inline constexpr std::size_t shell_socket_accept_budget = 8;
// 遠端 Session 一輪傳送處理的次數預算，不是 byte 容量。
inline constexpr int shell_socket_send_budget = 8;
// 遠端 Shell 一次 socket 讀取的緩衝容量（bytes）。
inline constexpr std::size_t shell_socket_read_bytes = 4096;
// 遠端 Shell 一次 poller 等待最多取回的就緒事件數。
inline constexpr std::size_t shell_socket_ready_events = 16;
// Socket Shell 事件迴圈等待／補查的間隔（毫秒）。
inline constexpr auto shell_socket_poll_interval = std::chrono::milliseconds(20);
// Suspend listener polling while resource pressure persists; retry without a busy loop.
// listener 遇到資源壓力後暫停接受連線的初始退避（毫秒）。
inline constexpr auto shell_socket_accept_retry_initial = std::chrono::milliseconds(100);
// listener 接受連線失敗後逐步退避的最大間隔（毫秒）。
inline constexpr auto shell_socket_accept_retry_max = std::chrono::milliseconds(5000);
// Socket Shell 重複故障診斷的回報間隔（秒），避免持續洗版。
inline constexpr auto shell_socket_diagnostic_interval = std::chrono::seconds(5);
// Socket Shell 關閉時嘗試送完待送文字的最後寬限（秒）。
inline constexpr auto shell_socket_final_drain_timeout = std::chrono::seconds(2);

// 【Tcl 腳本】輸出收集、交付及最終結果分別有自己的緩衝。
// Tcl output, delivered output and result messages have separate bounded buffers.
// Tcl worker 待取輸出緩衝的容量上限（bytes），不是整支腳本的總輸出量。
inline constexpr std::size_t shell_script_output_bytes = 256 * 1024;
// 腳本輸出交付緩衝的容量上限（bytes），與收集及最終結果緩衝分開。
inline constexpr std::size_t shell_script_delivery_bytes = 256 * 1024;
// 腳本最終結果文字的容量上限（bytes），不是所有腳本輸出的總容量。
inline constexpr std::size_t shell_script_result_bytes = 256 * 1024;
// 最終結果緩衝保留給截斷提示尾碼的空間（bytes）。
inline constexpr std::size_t shell_script_result_suffix_reserve = 32;
// 腳本等待期間檢查停止／取消的時間切片（毫秒）。
inline constexpr auto shell_script_wait_slice = std::chrono::milliseconds(20);

// Bound diagnostic delivery before a child closes its control connection.
// Ordinary Socket error acknowledgement only; fatal shared records use no delivery timeout.
// 一般 Socket 錯誤回報關閉前等待確認的期限（毫秒）；不套用到 fatal 共享紀錄。
inline constexpr auto module_error_report_timeout = std::chrono::milliseconds(500);

// 【Log】單筆文字、檔案輪替、佇列容量、重要紀錄保留與收集週期。
// Log budgets count encoded records/bytes, not filesystem allocation units.
// 單筆結構化 Log 文字的長度上限（bytes），也用於原始輸出長行的分段。
inline constexpr std::size_t log_message_bytes = 8 * 1024;
// 每個受管理 Log 檔案的預設大小上限（bytes），可由 FileOptions 覆寫。
inline constexpr std::size_t log_file_bytes = 16 * 1024 * 1024;
// 所有受管理日期目錄內 Log 檔案的預設合計上限（bytes）。
inline constexpr std::size_t log_total_bytes = 128 * 1024 * 1024;
// 中央待寫入磁碟佇列的筆數上限；顯示歷史亦使用同樣的筆數容量。
inline constexpr std::size_t log_queue_records = 1024;
// 中央待寫入磁碟佇列的文字／metadata 預算（bytes）；顯示歷史另有同額預算。
inline constexpr std::size_t log_queue_bytes = 8 * 1024 * 1024;
// 中央磁碟佇列為 Error／Warn 保留的筆數；只保護此佇列，不保證端到端送達。
inline constexpr std::size_t log_reserved_records = 64;
// 中央磁碟佇列為 Error／Warn 保留的容量（bytes），不改變 FIFO 次序。
inline constexpr std::size_t log_reserved_bytes = 512 * 1024;
// Host Log 控制 Session 為 Error／Warn 等重要傳送保留的 frame 數。
inline constexpr std::size_t log_session_reserved_records = 8;
// Host Log 控制 Session 的重要傳送保留容量（bytes）。
inline constexpr std::size_t log_session_reserved_bytes = 32 * 1024;
// Log 每來源每輪收集／交付的紀錄筆數預算，避免單一來源長期獨占處理。
inline constexpr std::size_t log_collection_budget = 16;
// Shell 一次取回 Log 顯示文字的大小上限（bytes），不是磁碟檔案上限。
inline constexpr std::size_t log_display_bytes = 64 * 1024;
// 中央 Log collector 等待／收集的間隔（毫秒）。
inline constexpr std::chrono::milliseconds log_poll_interval{20};
// Log disk worker 定期 flush 的間隔（毫秒），不等於每筆都有斷電保存保證。
inline constexpr std::chrono::milliseconds log_flush_interval{1000};
// Log 關閉收尾的等待期限（毫秒），不是整個框架的關閉上限。
inline constexpr std::chrono::milliseconds log_shutdown_timeout{1000};
// Host 建立獨立 Log 連線時的等待期限（毫秒）；失敗不阻止 Module 繼續啟動。
inline constexpr std::chrono::milliseconds log_connect_timeout{1000};
// Host 重新回報非零 Log 拒收統計的間隔（毫秒）。
inline constexpr std::chrono::milliseconds log_statistics_interval{1000};
} // namespace vinc::config

namespace vinc::config {
// 【Bag Recorder】待寫入預算、SQLite 主檔容量、交易批次與檢視工作量。
// Recorder pending payload budget includes the writer's current transaction.
// Recorder 尚未完成寫入的 payload 預設總預算（bytes），包含 writer 當前交易。
inline constexpr std::uint64_t bag_queue_bytes = 256ULL * 1024 * 1024;
// Recorder 待寫入紀錄數的預設上限，用來限制大量小訊息的簿記成本。
inline constexpr std::size_t bag_queue_records = 4096;
// Bag 單筆完整訊息映像的容量上限（bytes），亦為 Recorder 的預設接收容量。
inline constexpr std::uint64_t bag_message_bytes = 128ULL * 1024 * 1024 + 1024 * 1024;
// Main database bytes only; transaction journal space is additional.
// 允許指定的 SQLite 主資料庫容量下限（bytes）。
inline constexpr std::uint64_t bag_min_database_bytes = 4ULL * 1024 * 1024;
// SQLite 主資料庫的預設容量上限（bytes）；含 metadata／索引，交易暫存檔另計。
inline constexpr std::uint64_t bag_database_bytes = 16ULL * 1024 * 1024 * 1024;
// writer 組成單次交易批次時的 payload 大小門檻（bytes），不是單筆訊息硬上限。
inline constexpr std::uint64_t bag_batch_bytes = 16ULL * 1024 * 1024;
// writer 單次交易批次最多納入的紀錄數。
inline constexpr std::size_t bag_batch_records = 256;
// Recorder 累積資料等待成批寫入的最長定時等待（毫秒），不是硬性落盤延遲保證。
inline constexpr auto bag_flush_interval = std::chrono::milliseconds{100};
// Recorder 重新探索可錄製 Topic／Publisher 的間隔（毫秒）。
inline constexpr auto bag_discovery_interval = std::chrono::milliseconds{500};
// Recorder Module 的 Manual 主迴圈處理 pump 與錄製後的休眠間隔（毫秒）。
inline constexpr auto bag_pump_interval = std::chrono::milliseconds{10};
// Bag 錄製／檢視所允許的不同 Topic 數量上限。
inline constexpr std::size_t bag_max_topics = 1024;
// Bag 檢視查詢最多收集的不同發布來源數，依發布者名稱與 Publisher ID 彙整。
inline constexpr std::size_t bag_max_sources = 65536;
// SQLite 頁面快取的設定預算（KiB），不是主資料庫容量。
inline constexpr int bag_sqlite_cache_kib = 8192;
// SQLite 遇到資料庫鎖競爭時的忙碌重試期限（毫秒）。
inline constexpr int bag_sqlite_busy_timeout_ms = 2000;
// Recorder Module 正常停止並完成 writer 收尾的等待設定（秒）。
inline constexpr auto bag_stop_timeout = std::chrono::seconds{30};
// Bag 檢視查詢每執行多少 SQLite VM 指令，就呼叫一次工作量檢查。
inline constexpr int bag_info_progress_interval = 1000;
// Bag 檢視查詢允許的進度檢查呼叫數門檻；超出便中止，並非資料列數或時間。
inline constexpr std::uint64_t bag_info_progress_calls = 100000;
// Bag Topic、型別等名稱欄位的長度上限（bytes）。
inline constexpr std::size_t bag_name_bytes = 256;
}

namespace vinc::config {
// 【Python】未處理 hook 例外的診斷輸出。
// Maximum diagnostic text emitted for one unhandled Python exception.
// 單次未處理 Python hook 例外可輸出的 traceback 診斷長度上限（bytes）。
inline constexpr std::size_t python_traceback_bytes = 32 * 1024;
}

namespace vinc::config {
// 【Web UI】啟動資源快照、plugin 套件、HTTP worker 與服務啟停。
// UI keeps an immutable startup snapshot; browsers cannot grow this budget.
// 官方 UI Module 的預設 HTTP 監聽埠；預設位址為本機 loopback。
inline constexpr std::uint16_t ui_port = 8080;
// UI 啟動資源快照中單一檔案的大小上限（bytes）。
inline constexpr std::size_t ui_file_bytes = 8 * 1024 * 1024;
// UI 啟動讀入全部資源的合計內容預算（bytes），不包含索引、allocator 與 HTTP 回覆副本。
inline constexpr std::size_t ui_snapshot_bytes = 64 * 1024 * 1024;
// 每個 UI app／plugin 套件可掃描的目錄項目數上限。
inline constexpr std::size_t ui_package_entries = 1024;
// UI plugins 根目錄可掃描的套件目錄項目數上限。
inline constexpr std::size_t ui_plugin_limit = 64;
// 單一 UI plugin 可在 manifest 宣告的 panel 類型數上限，不是已開分頁數。
inline constexpr std::size_t ui_panels_per_plugin = 16;
// UI plugin manifest 檔案的大小上限（bytes）。
inline constexpr std::size_t ui_manifest_bytes = 16 * 1024;
// UI plugin／panel 識別名稱的長度上限（bytes）。
inline constexpr std::size_t ui_identifier_bytes = 80;
// UI manifest 中顯示文字欄位的長度上限（bytes）。
inline constexpr std::size_t ui_label_bytes = 128;
// UI HTTP 固定 worker 執行緒數，不會因連線變多自動擴充。
inline constexpr std::size_t ui_http_workers = 2;
// UI HTTP worker 佇列最多保留的等待工作數，滿載時關閉額外連線。
inline constexpr std::size_t ui_http_queue = 16;
// HTTP 讀／寫等待的逾時設定（秒），不是整個 request 的總期限。
inline constexpr auto ui_http_timeout = std::chrono::seconds{2};
// 等待 HTTP listener 啟動就緒的期限（秒）。
inline constexpr auto ui_start_timeout = std::chrono::seconds{5};
// 等待 HTTP listener 就緒時的狀態觀察間隔（毫秒）。
inline constexpr auto ui_start_poll_interval = std::chrono::milliseconds{5};
// UI Module 正常停止的寬限設定（秒）；無回應時沿用 Module 強制退出政策。
inline constexpr auto ui_stop_timeout = std::chrono::seconds{10};
// 【Topic Monitor】診斷燈號、發布頻率與訊息預覽；只影響觀察，不改變 Bus 行為。
// Topic diagnostics affect observation only, never writer waits or module policy.
// Topic 診斷與發布統計的採樣間隔（秒），不是訊息傳輸期限。
inline constexpr auto ui_topic_sample_interval = std::chrono::seconds{1};
// Topic 清單／預覽快取超過多久視為不可用（秒），HTTP 仍可連線也不保留舊綠燈。
inline constexpr auto ui_topic_stale_timeout = std::chrono::seconds{3};
// 計算 Publisher Hz 的滾動觀察窗口（秒）；依發布序號與單調時間計算。
inline constexpr auto ui_topic_rate_window = std::chrono::seconds{5};
// Topic 不再發生新問題後，恢復正常燈號所需的連續觀察時間（秒）。
inline constexpr auto ui_topic_recovery = std::chrono::seconds{5};
// Writer 當前持續競爭等待達此時間便顯示紅燈（秒）；不會替 Bus 增加逾時。
inline constexpr auto ui_topic_red_wait = std::chrono::seconds{1};
// UI 預覽工作各輪之間的取樣間隔（毫秒），避免 HTTP 刷新頻率直接放大複製工作。
inline constexpr auto ui_preview_interval = std::chrono::milliseconds{500};
// UI Module Manual 主迴圈閒置時的補查間隔（毫秒），不是 HTTP request timeout。
inline constexpr auto ui_monitor_poll_interval = std::chrono::milliseconds{20};
// 預覽 Topic 多久未再被請求便關閉其 Reader（秒）。
inline constexpr auto ui_preview_idle_timeout = std::chrono::seconds{5};
// Topic Monitor 可同時維護的 Topic 數量上限。
inline constexpr std::size_t ui_monitor_topics = 256;
// 每個發布來源的 Hz 滾動計算最多保留的統計樣本數。
inline constexpr std::size_t ui_rate_samples = 8;
// UI 可同時維護的訊息預覽 Topic 數量上限。
inline constexpr std::size_t ui_preview_topics = 8;
// 預覽可接收的完整訊息映像上限（bytes）；超出只停用預覽，不代表訊息損壞。
inline constexpr std::size_t ui_preview_message_bytes = 16 * 1024 * 1024;
// 單筆訊息預覽產生的文字內容總上限（bytes）。
inline constexpr std::size_t ui_preview_text_bytes = 16 * 1024;
// 預覽每個陣列最多展開的元素數，超出內容省略顯示。
inline constexpr std::size_t ui_preview_elements = 32;
// 單筆預覽最多走訪的資料節點數，限制大型巢狀資料的格式化工作。
inline constexpr std::size_t ui_preview_nodes = 256;
// 訊息預覽最多展開的巢狀深度。
inline constexpr std::size_t ui_preview_depth = 16;
// 預覽單一字串最多顯示的內容長度（bytes）。
inline constexpr std::size_t ui_preview_string_bytes = 256;
// Topic Monitor 序列化清單快取的總大小上限（bytes），超出回報不可用。
inline constexpr std::size_t ui_topic_snapshot_bytes = 4 * 1024 * 1024;
// 【Module 關係圖】使用者手動刷新的查詢期限及快照大小限制。
// Manual graph queries: one end-to-end deadline and complete-response limits.
// 使用者手動刷新關係圖的整次端到端期限（秒），包含等待信箱與查詢工作。
inline constexpr auto ui_graph_timeout = std::chrono::seconds{10};
// 單次關係圖快照最多收集的 Module 紀錄數。
inline constexpr std::size_t ui_graph_modules = 256;
// 單次關係圖快照最多收集的不同 Topic 數量。
inline constexpr std::size_t ui_graph_topics = 1024;
// 關係圖原始發布／訂閱登記數上限，在相同邊合併前計算。
inline constexpr std::size_t ui_graph_connections = 16384;
// 單次關係圖完整 JSON 回覆的大小上限（bytes）。
inline constexpr std::size_t ui_graph_response_bytes = 8 * 1024 * 1024;
// UI browser capability token 使用的 OS 亂數長度（bytes），不是登入密碼或中央管理權限。
inline constexpr std::size_t ui_session_token_bytes = 32;
// UI Topic 查詢／預覽接受的 Topic 名稱長度上限（bytes）。
inline constexpr std::size_t ui_topic_name_bytes = 256;
}

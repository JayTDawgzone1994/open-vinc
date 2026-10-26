#pragma once

// Temporary home for centralized VINC system settings.
// Runtime defaults will later be managed by Param; layout settings stay compile-time.
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace vinc::config {
// RPC admission, storage and scheduling bounds; see rpc/README.md.
inline constexpr std::uint64_t rpc_request_bytes = 64 * 1024;
inline constexpr std::uint64_t rpc_response_bytes = 64 * 1024;
inline constexpr std::uint32_t rpc_slots = 8;
inline constexpr std::uint32_t rpc_slot_limit = 256;
inline constexpr std::uint64_t rpc_shared_bytes = 256ULL * 1024 * 1024;
inline constexpr std::uint64_t rpc_local_bytes = 64ULL * 1024 * 1024;
inline constexpr std::size_t rpc_pending_limit = 64;
inline constexpr std::size_t rpc_service_limit = 256;
inline constexpr std::size_t rpc_control_limit = 4096;
// Leave control-queue headroom for grants, completions, releases and registration.
inline constexpr std::size_t rpc_active_limit = 1024;
inline constexpr std::size_t rpc_name_bytes = 128;
inline constexpr std::size_t rpc_reason_bytes = 256;
inline constexpr auto rpc_call_timeout = std::chrono::seconds(5);
inline constexpr auto rpc_max_call_timeout = std::chrono::hours(24);
inline constexpr auto rpc_control_retry = std::chrono::milliseconds(5);

// Param admission and persistence bounds; existing framework settings are not migrated to Param.
inline constexpr std::size_t param_name_bytes = 256;
inline constexpr std::size_t param_string_bytes = 256;
inline constexpr std::size_t param_entry_limit = 1024;
inline constexpr std::size_t param_pending_limit = 32;
inline constexpr std::size_t param_queue_limit = 256;
inline constexpr std::size_t param_slot_bytes = 1024 * 1024;
inline constexpr std::size_t param_file_bytes = 4 * 1024 * 1024;
inline constexpr auto param_wait_poll_interval = std::chrono::milliseconds(10); // Active caller cancellation checks.
inline constexpr auto param_request_timeout = std::chrono::seconds(10);
inline constexpr auto param_lock_timeout = std::chrono::seconds(2);
// Bounds for central admission and its process-crash resource journal.
inline constexpr std::size_t instance_name_attempts = 32;
// Retained prefix reservations bound disk metadata and startup scanning.
inline constexpr std::size_t instance_epoch_limit = 65536;
inline constexpr std::size_t instance_resource_limit = 16384;
inline constexpr std::size_t module_lock_limit = 64;
inline constexpr std::size_t module_lock_pending_limit = 32;
// Shared mutex polling sleeps locally; cancellation wakes the sleep. This is a
// retry policy, not an OS scheduling or hard completion guarantee.
inline constexpr auto concurrency_retry_interval = std::chrono::milliseconds(5);
// Idle data polling and Host supervision of a locally completed/failed main.
inline constexpr std::chrono::milliseconds module_pump_idle_interval{10};
inline constexpr std::chrono::milliseconds module_runtime_poll_interval{20};
inline constexpr std::size_t module_callback_limit = 256;


// Central's grace after first observing a fatal report. Earlier startup or
// explicit-stop deadlines win; expiry requests OS termination, not proof of exit.
inline constexpr auto module_fatal_exit_timeout = std::chrono::seconds(5);
static_assert(module_fatal_exit_timeout > std::chrono::seconds::zero());

// Total stored error records, including the current error. This changes C++ layout;
// every linked consumer must use the same value and be rebuilt after a change.
// Unlike runtime settings, this capacity cannot be changed by Param.
inline constexpr std::size_t error_chain_max_depth = 3;
static_assert(error_chain_max_depth >= 2, "Error chains require at least two records");

// Message Bus defaults. Byte budgets include the complete encoded message image.
inline constexpr std::uint32_t message_bus_default_slots = 2;
inline constexpr std::uint64_t message_bus_default_message_bytes = 1024 * 1024;
inline constexpr std::uint32_t message_bus_default_topics = 128;
inline constexpr std::uint32_t message_bus_default_publishers_per_topic = 32;
inline constexpr std::uint32_t message_bus_default_subscribers_per_topic = 128;
inline constexpr std::uint64_t message_bus_default_shared_bytes = 1024ULL * 1024 * 1024;

// Control backpressure and the deadline for an unanswered registration request.
inline constexpr std::size_t message_bus_pending_requests = 32;
inline constexpr std::size_t message_bus_manager_queue_size = 256;
inline constexpr auto message_bus_request_timeout = std::chrono::seconds(10);

// Retry policy only: protected reader copies still have no contention timeout.
// The pre-increment comparison yields for attempts 1..63, then sleeps.
inline constexpr std::uint32_t message_bus_reader_sleep_threshold = 64;
inline constexpr auto message_bus_reader_retry_interval = std::chrono::milliseconds(1);

// OutputHub queue and capture defaults. Byte limits count raw payload bytes.
inline constexpr std::size_t output_hub_default_queue_bytes = 256 * 1024;
inline constexpr std::size_t output_hub_default_queue_events = 128;
inline constexpr std::size_t output_hub_default_captures = 128;
inline constexpr std::size_t output_hub_default_subscribers = 8;
inline constexpr std::size_t output_hub_default_total_queue_bytes = 2 * 1024 * 1024;
inline constexpr std::size_t output_hub_default_total_queue_events = 1024;
inline constexpr auto output_hub_default_drain_timeout = std::chrono::milliseconds(2000);
inline constexpr std::size_t output_hub_default_take_events = 32;

// Admission limits and worker budgets. Shutdown and tail drain are separate deadlines.
inline constexpr std::size_t output_hub_max_captures = 1024;
inline constexpr std::size_t output_hub_max_subscribers = 64;
inline constexpr std::size_t output_hub_max_source_name_bytes = 256;
inline constexpr auto output_hub_max_drain_timeout = std::chrono::seconds(60);
inline constexpr std::size_t output_hub_read_chunk_bytes = 4096;
inline constexpr std::size_t output_hub_completion_batch_size = 32;
inline constexpr auto output_hub_poll_interval = std::chrono::milliseconds(20);
inline constexpr auto output_hub_failure_backoff = std::chrono::milliseconds(20);
inline constexpr auto output_hub_shutdown_timeout = std::chrono::seconds(2);

// Text presentation limits are independent of the raw Pipe read chunk size.
inline constexpr std::size_t output_hub_text_max_sources = 256;
inline constexpr std::size_t output_hub_text_line_bytes = 4096;
inline constexpr auto output_hub_text_flush_interval = std::chrono::milliseconds(100);

// Local Shell admission limits; byte limits apply before parsing or editing.
inline constexpr std::size_t shell_max_line_bytes = 4096;
inline constexpr std::size_t shell_max_arguments = 256;
inline constexpr std::size_t shell_history_entries = 128;
inline constexpr std::size_t shell_max_jobs = 16;

// Keep output batches bounded so command results and keyboard input get a turn.
inline constexpr std::size_t shell_output_batch_events = 16;
inline constexpr std::size_t shell_final_output_batch_events = 128;
inline constexpr auto shell_command_timeout = std::chrono::seconds(5);
inline constexpr auto shell_completion_timeout = std::chrono::seconds(1);
inline constexpr auto shell_terminal_poll_interval = std::chrono::milliseconds(20);
inline constexpr auto shell_headless_backoff = std::chrono::milliseconds(20);

// Shutdown has no overall timeout. These intervals control collection and diagnostics.
inline constexpr auto shell_shutdown_poll_interval = std::chrono::milliseconds(250);
inline constexpr auto shell_shutdown_report_interval = std::chrono::seconds(1);
inline constexpr auto shell_job_poll_interval = std::chrono::milliseconds(10);

// Remote Shell defaults. Timeouts measure disconnected ownership and stalled sends,
// not the lifetime of an idle, healthy TCP connection.
inline constexpr char shell_socket_default_ip[] = "127.0.0.1";
inline constexpr std::uint16_t shell_socket_default_port = 5500;
inline constexpr auto shell_socket_disconnect_timeout = std::chrono::milliseconds(30000);
inline constexpr auto shell_socket_send_timeout = std::chrono::milliseconds(30000);
inline constexpr std::size_t shell_socket_max_connections = 8;
// One listener plus live clients. Retained sessions also cover reconnecting owners;
// an extra provisional session can restore an owner when ordinary storage is full.
inline constexpr std::size_t shell_socket_poller_capacity = shell_socket_max_connections + 1;
inline constexpr std::size_t shell_socket_retained_session_factor = 2;
inline constexpr std::size_t shell_socket_resume_slots = 1;
inline constexpr std::size_t shell_script_mailboxes =
    shell_socket_max_connections * shell_socket_retained_session_factor + 1; // Local owner included.

// Network queue bytes count escaped wire text; the payload limit applies before escaping.
inline constexpr std::size_t shell_socket_queue_bytes = 1024 * 1024;
inline constexpr std::size_t shell_socket_payload_bytes = 256 * 1024;
inline constexpr std::size_t shell_socket_reply_reserve_bytes = 600 * 1024;
inline constexpr std::size_t shell_socket_queue_items = 128;
inline constexpr std::size_t shell_socket_collect_threshold_items = 120;
inline constexpr std::size_t shell_socket_output_queue_bytes = 128 * 1024;
inline constexpr std::size_t shell_socket_output_queue_events = 64;
inline constexpr std::size_t shell_socket_output_batch_events = 4;
inline constexpr std::size_t shell_socket_accept_budget = 8;
inline constexpr int shell_socket_send_budget = 8;
inline constexpr std::size_t shell_socket_read_bytes = 4096;
inline constexpr std::size_t shell_socket_ready_events = 16;
inline constexpr auto shell_socket_poll_interval = std::chrono::milliseconds(20);
// Suspend listener polling while resource pressure persists; retry without a busy loop.
inline constexpr auto shell_socket_accept_retry_initial = std::chrono::milliseconds(100);
inline constexpr auto shell_socket_accept_retry_max = std::chrono::milliseconds(5000);
inline constexpr auto shell_socket_diagnostic_interval = std::chrono::seconds(5);
inline constexpr auto shell_socket_final_drain_timeout = std::chrono::seconds(2);

// Tcl output, delivered output and result messages have separate bounded buffers.
inline constexpr std::size_t shell_script_output_bytes = 256 * 1024;
inline constexpr std::size_t shell_script_delivery_bytes = 256 * 1024;
inline constexpr std::size_t shell_script_result_bytes = 256 * 1024;
inline constexpr std::size_t shell_script_result_suffix_reserve = 32;
inline constexpr auto shell_script_wait_slice = std::chrono::milliseconds(20);

// Bound diagnostic delivery before a child closes its control connection.
// Ordinary Socket error acknowledgement only; fatal shared records use no delivery timeout.
inline constexpr auto module_error_report_timeout = std::chrono::milliseconds(500);

// Log budgets count encoded records/bytes, not filesystem allocation units.
inline constexpr std::size_t log_message_bytes = 8 * 1024;
inline constexpr std::size_t log_file_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t log_total_bytes = 128 * 1024 * 1024;
inline constexpr std::size_t log_queue_records = 1024;
inline constexpr std::size_t log_queue_bytes = 8 * 1024 * 1024;
inline constexpr std::size_t log_reserved_records = 64;
inline constexpr std::size_t log_reserved_bytes = 512 * 1024;
inline constexpr std::size_t log_session_reserved_records = 8;
inline constexpr std::size_t log_session_reserved_bytes = 32 * 1024;
inline constexpr std::size_t log_collection_budget = 16;
inline constexpr std::size_t log_display_bytes = 64 * 1024;
inline constexpr std::chrono::milliseconds log_poll_interval{20};
inline constexpr std::chrono::milliseconds log_flush_interval{1000};
inline constexpr std::chrono::milliseconds log_shutdown_timeout{1000};
inline constexpr std::chrono::milliseconds log_connect_timeout{1000};
inline constexpr std::chrono::milliseconds log_statistics_interval{1000};
} // namespace vinc::config

namespace vinc::config {
// Recorder pending payload budget includes the writer's current transaction.
inline constexpr std::uint64_t bag_queue_bytes = 256ULL * 1024 * 1024;
inline constexpr std::size_t bag_queue_records = 4096;
inline constexpr std::uint64_t bag_message_bytes = 128ULL * 1024 * 1024 + 1024 * 1024;
// Main database bytes only; transaction journal space is additional.
inline constexpr std::uint64_t bag_min_database_bytes = 4ULL * 1024 * 1024;
inline constexpr std::uint64_t bag_database_bytes = 16ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t bag_batch_bytes = 16ULL * 1024 * 1024;
inline constexpr std::size_t bag_batch_records = 256;
inline constexpr auto bag_flush_interval = std::chrono::milliseconds{100};
inline constexpr auto bag_discovery_interval = std::chrono::milliseconds{500};
inline constexpr auto bag_pump_interval = std::chrono::milliseconds{10};
inline constexpr std::size_t bag_max_topics = 1024;
inline constexpr std::size_t bag_max_sources = 65536;
inline constexpr int bag_sqlite_cache_kib = 8192;
inline constexpr int bag_sqlite_busy_timeout_ms = 2000;
inline constexpr auto bag_stop_timeout = std::chrono::seconds{30};
inline constexpr int bag_info_progress_interval = 1000;
inline constexpr std::uint64_t bag_info_progress_calls = 100000;
inline constexpr std::size_t bag_name_bytes = 256;
}

namespace vinc::config {
// Maximum diagnostic text emitted for one unhandled Python exception.
inline constexpr std::size_t python_traceback_bytes = 32 * 1024;
}

namespace vinc::config {
// UI keeps an immutable startup snapshot; browsers cannot grow this budget.
inline constexpr std::uint16_t ui_port = 8080;
inline constexpr std::size_t ui_file_bytes = 8 * 1024 * 1024;
inline constexpr std::size_t ui_snapshot_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t ui_package_entries = 1024;
inline constexpr std::size_t ui_plugin_limit = 64;
inline constexpr std::size_t ui_panels_per_plugin = 16;
inline constexpr std::size_t ui_manifest_bytes = 16 * 1024;
inline constexpr std::size_t ui_identifier_bytes = 80;
inline constexpr std::size_t ui_label_bytes = 128;
inline constexpr std::size_t ui_http_workers = 2;
inline constexpr std::size_t ui_http_queue = 16;
inline constexpr auto ui_http_timeout = std::chrono::seconds{2};
inline constexpr auto ui_start_timeout = std::chrono::seconds{5};
inline constexpr auto ui_start_poll_interval = std::chrono::milliseconds{5};
inline constexpr auto ui_stop_timeout = std::chrono::seconds{10};
// Topic diagnostics affect observation only, never writer waits or module policy.
inline constexpr auto ui_topic_sample_interval = std::chrono::seconds{1};
inline constexpr auto ui_topic_stale_timeout = std::chrono::seconds{3};
inline constexpr auto ui_topic_rate_window = std::chrono::seconds{5};
inline constexpr auto ui_topic_recovery = std::chrono::seconds{5};
inline constexpr auto ui_topic_red_wait = std::chrono::seconds{1};
inline constexpr auto ui_preview_interval = std::chrono::milliseconds{500};
inline constexpr auto ui_monitor_poll_interval = std::chrono::milliseconds{20};
inline constexpr auto ui_preview_idle_timeout = std::chrono::seconds{5};
inline constexpr std::size_t ui_monitor_topics = 256;
inline constexpr std::size_t ui_rate_samples = 8;
inline constexpr std::size_t ui_preview_topics = 8;
inline constexpr std::size_t ui_preview_message_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t ui_preview_text_bytes = 16 * 1024;
inline constexpr std::size_t ui_preview_elements = 32;
inline constexpr std::size_t ui_preview_nodes = 256;
inline constexpr std::size_t ui_preview_depth = 16;
inline constexpr std::size_t ui_preview_string_bytes = 256;
inline constexpr std::size_t ui_topic_snapshot_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t ui_session_token_bytes = 32;
inline constexpr std::size_t ui_topic_name_bytes = 256;
}

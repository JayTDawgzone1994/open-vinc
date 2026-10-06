#pragma once

// Private lifecycle and command operation numbers shared by central and child runtimes.
// Keep these identities aligned on both ends of the control channel.

#include <vinc/control/protocol.hpp>

namespace vinc::detail {
constexpr std::uint16_t lifecycle = static_cast<std::uint16_t>(control::Domain::Lifecycle);
constexpr std::uint16_t commands = static_cast<std::uint16_t>(control::Domain::Command);
constexpr std::uint16_t ready = 1;
constexpr std::uint16_t stop = 2;
constexpr std::uint16_t register_name = 3;
constexpr std::uint16_t report_failure = 4;
constexpr std::uint16_t debug_waiting = 5;
constexpr std::uint16_t debug_continue = 6;
constexpr std::uint16_t finishing = 7;
constexpr std::uint16_t completed = 8;
constexpr std::uint64_t completion_request = UINT64_MAX;
constexpr std::uint32_t primary_failure = 0;
constexpr std::uint32_t cleanup_failure = 1;
constexpr std::uint16_t execute = 1;
constexpr std::uint16_t complete = 2;
constexpr std::uint64_t name_registration_request = 1;
constexpr std::size_t command_status_bytes = sizeof(std::uint32_t);

// Positions exclude argv[0]. Changing this layout requires both launch endpoints to change.
namespace bootstrap {
constexpr char version_marker[] = "--vinc-module-v7";
constexpr std::size_t marker = 0;
constexpr std::size_t endpoint = 1;
constexpr std::size_t base_name = 2;
constexpr std::size_t token = 3;
constexpr std::size_t generation = 4;
constexpr std::size_t heartbeat_interval = 5;
constexpr std::size_t heartbeat_timeout = 6;
constexpr std::size_t startup_timeout = 7;
constexpr std::size_t stop_timeout = 8;
constexpr std::size_t module_type = 9;
constexpr std::size_t fault_storage = 10;
constexpr std::size_t plugin_path = 11;
constexpr std::size_t debug = 12;
constexpr std::size_t argument_count = 13;
}
} // namespace vinc::detail

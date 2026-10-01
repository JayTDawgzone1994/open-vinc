#pragma once

// Private lifecycle and command operation numbers shared by central and child runtimes.
// Keep these identities aligned on both ends of the control channel.

#include <vinc/control/protocol.hpp>
namespace vinc::detail {
constexpr std::uint16_t lifecycle = static_cast<std::uint16_t>(control::Domain::Lifecycle);
constexpr std::uint16_t commands = static_cast<std::uint16_t>(control::Domain::Command);
constexpr std::uint16_t ready = 1, stop = 2;
constexpr std::uint16_t execute = 1, complete = 2;
constexpr int32_t unsupported_operation = -110, hook_exception = -111;
}

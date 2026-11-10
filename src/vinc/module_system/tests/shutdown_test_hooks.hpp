#pragma once

#include <vinc/control/protocol.hpp>
#include <vinc/error.hpp>
#include <source_location>

// Only the isolated shutdown Host defines these hooks. They are absent from
// production libraries and do not add a runtime registry or a public ABI.
namespace vinc::shutdown_test {
void hooks_result(const Result<void>& result);
void runtime_result(const Result<void>& result);
void after_finished_check(Result<control::Frame>& frame);
void bus_result(const Result<void>& result);
void before_watchdog_join();
void watchdog_exited();
void before_bus_shutdown();
void after_bus_shutdown();
void watchdog_failure(const Error& error, int exit_code);
// Called with the Bus mutex held: diagnostics only; never wait on a test gate.
void first_bus_close(std::source_location location);
void bus_receive(bool closed, const control::Frame& frame);
}

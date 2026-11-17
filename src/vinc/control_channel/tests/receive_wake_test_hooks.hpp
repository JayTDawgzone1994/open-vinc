#pragma once

#include <vinc/control/detail/receive_wake.hpp>
#include <vinc/control/protocol.hpp>

namespace vinc::control::test {
void before_receive_wait(const detail::ReceiveWake* wake);
void before_receive_signal(const detail::ReceiveWake* wake);
void received(std::uint64_t generation, Kind kind);
}

namespace vinc::control::detail {
struct ReceiveWakeTestAccess {
    // A deliberate spurious wake: no pending state is published.
    static void spurious(ReceiveWake& wake) {
        std::lock_guard lock(wake.mutex_);
        wake.changed_.notify_all();
    }
};
}

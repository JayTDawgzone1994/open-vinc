#pragma once
#include <cstdint>

namespace vinc {
struct PumpOptions {
    // Zero retains caller-thread dispatch. Positive values add Topic workers;
    // the caller remains the serialized Param/Shell/RPC dispatcher.
    std::uint32_t topic_workers = 0;
};
}

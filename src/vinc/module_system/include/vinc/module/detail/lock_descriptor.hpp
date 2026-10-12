#pragma once
#include <cstdint>
#include <string>

namespace vinc::detail {
struct LockDescriptor {
    std::uint64_t id = 0;
    std::string name;
};
}

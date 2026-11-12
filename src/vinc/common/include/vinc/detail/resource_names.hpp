#pragma once

#include <vinc/result.hpp>
#include <string>
#include <string_view>

namespace vinc::detail {
// Injected by the central owner. No global registry and no OS dependency here.
// reserve() records an exclusive name BEFORE its native resource is created.
// retire() removes only the caller's journal reservation. It is valid after
// confirmed native name cleanup, Windows retirement, or a failed exclusive
// create that confirms no owned name remains. Uncertain cleanup retains the record.
class ResourceNames {
public:
    virtual ~ResourceNames() = default;
    virtual Result<std::string> reserve() = 0;
    virtual Result<void> retire(std::string_view name) = 0;
};
}

#pragma once
#include <vinc/param/params.hpp>

namespace vinc::param::detail {
class ClientAccess {
public:
    static void bind(Params& params, Access& access) noexcept {
        params.access_ = &access;
    }
};
}

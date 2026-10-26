#pragma once
#include <vinc/param/types.hpp>
#include <optional>

namespace vinc::param::detail {
// Borrowed by Context, owned by ModuleHost. No transport implementation crosses
// into Plugin ownership; normal service calls do not use RuntimeServices.
class Access {
public:
    virtual ~Access() = default;
    virtual std::string_view module_name() const noexcept = 0;
    virtual Result<void> register_value(std::string name, Value defaults, Permission permission) = 0;
    virtual Result<void> set_value(std::string name, Value value) = 0;
    virtual Result<void> watch_value(std::string name, Type type) = 0;
    virtual Result<std::optional<Snapshot>> snapshot(std::uint64_t known_revision) = 0;
};
class ClientAccess;
}

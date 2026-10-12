#pragma once
#include <vinc/param/detail/access.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace vinc {
class Context;
}

namespace vinc::param {
struct WatchStatus {
    std::string name;
    Type expected;
    bool present = false;
    bool compatible = false;
};

// Context owns this facade. A successful mutation acknowledges central commit,
// not local cache refresh or persistence. Only pump refreshes the cache.
class Params {
    friend class vinc::Context;
    friend class detail::ClientAccess;
    Params() = default;
    detail::Access* access_ = nullptr;
    mutable std::mutex mutex_;
    std::map<std::string, Type, std::less<>> watched_;
    ParamMap cache_;
    std::shared_ptr<std::function<void(const ParamMap&)>> callback_;
    std::uint64_t revision_ = UINT64_MAX;
    std::uint64_t watch_revision_ = 0;
    std::uint64_t applied_watch_revision_ = UINT64_MAX;
    Result<void> watch(std::string name, Type type);
    Result<void> register_value(std::string name, Value defaults, Permission permission);
    Result<Value> read(std::string_view name) const;
    Result<std::uint64_t> pump();

public:
    Params(const Params&) = delete;
    Params& operator=(const Params&) = delete;

    template <ParamType T>
    Result<void> register_param(std::string name, T defaults,
                                Permission permission = Permission::FrameworkOnly) {
        return register_value(std::move(name), Value(std::move(defaults)), permission);
    }

    template <ParamType T> Result<void> register_param(std::string name) {
        return watch(std::move(name), type_of(Value(T{})));
    }

    template <ParamType T> Result<void> update(std::string_view name, T& destination) const {
        auto value = read(name);
        if (!value) {
            return std::unexpected(value.error());
        }
        auto* typed = std::get_if<T>(&*value);
        if (!typed) {
            return std::unexpected(Error{ErrorCode::TypeMismatch, ErrorOperation::Update});
        }
        destination = std::move(*typed);
        return {};
    }

    template <ParamType T> Result<void> set(std::string name, T value) {
        return set_value(std::move(name), Value(std::move(value)));
    }

    Result<void> set_value(std::string name, Value value);
    Result<void> on_change(std::function<void(const ParamMap&)> callback);
    std::vector<WatchStatus> watches() const;
};
}

#pragma once
#include <vinc/module/module_info.hpp>
#include <vinc/module/error.hpp>
#include <vinc/control/protocol.hpp>
#include <vinc/owner_id.hpp>

namespace vinc::detail {
inline constexpr std::uint32_t module_query_version = 1;
inline constexpr std::uint32_t modules_per_page = 8;
inline constexpr std::size_t query_name_bytes = 256;

inline Result<control::Bytes> encode_modules(const ModulePage& page) {
    control::Bytes bytes;
    control::put_u32(bytes, module_query_version);
    control::put_u32(bytes, static_cast<std::uint32_t>(page.modules.size()));
    for (const auto& module : page.modules) {
        control::put_u64(bytes, module.mid);
        auto name = control::put_string(bytes, module.name);
        if (!name) {
            return std::unexpected(wrap_error(module_system::ErrorCode::Protocol,
                module_system::ErrorOperation::ListModules, name.error()));
        }

        auto type = control::put_string(bytes, module.module_type);
        if (!type) {
            return std::unexpected(wrap_error(module_system::ErrorCode::Protocol,
                module_system::ErrorOperation::ListModules, type.error()));
        }

        control::put_u32(bytes, static_cast<std::uint32_t>(module.state));
    }
    control::put_u64(bytes, page.next);
    return bytes;
}

inline Result<ModulePage> decode_modules(std::span<const std::byte> bytes, MID after) {
    using module_system::ErrorCode;
    using module_system::ErrorOperation;
    control::Reader reader(bytes);
    const auto version = reader.u32();
    const auto count = reader.u32();
    if (version != module_query_version || count > modules_per_page) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListModules});
    }

    ModulePage page;
    auto previous = after;
    for (std::uint32_t index = 0; index < count; ++index) {
        ModuleInfo module;
        module.mid = reader.u64();
        module.name = reader.string();
        module.module_type = reader.string();
        const auto state = reader.u32();
        const auto valid_name = [](const std::string& name) {
            return name.size() <= query_name_bytes && name.find('\0') == std::string::npos;
        };
        if (module.mid <= previous || module.mid >= central_owner ||
            !valid_name(module.name) || module.module_type.empty() || !valid_name(module.module_type) ||
            state > static_cast<std::uint32_t>(ModuleState::Failed)) {
            return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListModules});
        }

        module.state = static_cast<ModuleState>(state);
        previous = module.mid;
        page.modules.push_back(std::move(module));
    }
    page.next = reader.u64();
    if (!reader.done() || (page.next != 0 && (count == 0 || page.next != previous))) {
        return std::unexpected(Error{ErrorCode::Protocol, ErrorOperation::ListModules});
    }

    return page;
}
}

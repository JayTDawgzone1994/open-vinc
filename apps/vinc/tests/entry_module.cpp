#include <vinc/module/base_module.hpp>
#include <vinc/osal/path.hpp>
#include <iostream>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

// Verify the real Host image rather than accepting an identical result from the
// old standalone ModuleHost. Arguments resembling the bootstrap remain user data.
class EntryModule final : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    EntryModule() : BaseModule("entry") {
    }

    vinc::Result<std::string> on_name(std::vector<std::string>& arguments) override {
        // Shell startup retains the ModuleType as argv[0] in the hook arguments.
        if (arguments.size() != 4 || arguments.front() != "EntryModule") {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                               vinc::module_system::ErrorOperation::NameHook});
        }

        auto name = arguments[1];
        arguments.erase(arguments.begin(), arguments.begin() + 2);
        return name;
    }

    vinc::Result<void> on_start(const std::vector<std::string>& arguments) override {
        auto executable = vinc::osal::executable_path();
        if (!executable) {
            return std::unexpected(executable.error());
        }

        if (arguments.size() != 2 || arguments[1] != "--vinc-module-v0") {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                               vinc::module_system::ErrorOperation::StartHook});
        }

        auto expected = vinc::osal::path_from_utf8(arguments[0]);
        if (!expected) {
            return std::unexpected(expected.error());
        }

        if (*executable != *expected) {
            return std::unexpected(vinc::Error{vinc::module_system::ErrorCode::InvalidArgument,
                                               vinc::module_system::ErrorOperation::StartHook});
        }

        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string>) override {
#ifdef _WIN32
        const auto pid = _getpid();
#else
        const auto pid = getpid();
#endif
        std::cout << "ENTRY_PID=" << pid << std::endl;
        return 0;
    }
};
REGISTER_MODULE(EntryModule, "EntryModule")

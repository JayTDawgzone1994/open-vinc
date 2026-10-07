#include <vinc/osal/file_lock.hpp>
#include <vinc/osal/pipe.hpp>
#include <vinc/osal/process.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/socket.hpp>
#include <vinc/osal/terminal.hpp>
#include <vinc/osal/random.hpp>
#include <vinc/osal/dynamic_library.hpp>
#include <vinc/osal/detail/error_catalog.hpp>
#include <iostream>
#include <type_traits>

static_assert(std::is_same_v<decltype(vinc::osal::ProcessService::create())::error_type, vinc::Error>);
static_assert(std::is_same_v<decltype(vinc::osal::PipeService::create())::error_type, vinc::Error>);
static_assert(std::is_same_v<decltype(vinc::osal::FileLock::open({}))::error_type, vinc::Error>);
static_assert(std::is_same_v<decltype(vinc::osal::IPv4Address::parse(""))::error_type, vinc::Error>);
static_assert(std::is_same_v<decltype(vinc::osal::Terminal::open())::error_type, vinc::Error>);
static_assert(std::is_same_v<decltype(vinc::osal::random_bytes({}))::error_type, vinc::Error>);
static_assert(std::is_same_v<decltype(vinc::osal::DynamicLibrary::open({}))::error_type, vinc::Error>);

int main() {
    using namespace vinc;
    const auto address = osal::IPv4Address::parse("");
    if (address) {
        return 1;
    }

    const auto& error = address.error();
    const auto text = error_to_string(error);
    if (!error.is(osal::ErrorCode::InvalidArgument) ||
        !error.is_operation(osal::ErrorOperation::ParseAddress) || error.native ||
        text.find("Invalid argument") == std::string::npos ||
        text.find("ParseAddress") == std::string::npos) {
        std::cerr << text << '\n';
        return 1;
    }

    const osal::FileLock closed;
    const auto locked = closed.try_lock();
    if (locked || !locked.error().is(osal::ErrorCode::Closed) ||
        !locked.error().is_operation(osal::ErrorOperation::LockFileLock)) {
        return 1;
    }

    // These existing OSAL identifiers are contiguous; check coverage without assuming
    // future catalogs in other domains also use contiguous values.
    for (std::int32_t code = 1; code <= static_cast<std::int32_t>(osal::ErrorCode::InvalidEncoding); ++code) {
        const auto formatted = error_to_string(Error::from_raw(ErrorDomain::Osal, code, 0));
        if (formatted.find("Unknown error") != std::string::npos) {
            return 1;
        }
    }
    for (std::int32_t operation = 1; operation <= static_cast<std::int32_t>(osal::ErrorOperation::CloseLibrary);
         ++operation) {
        const auto formatted = error_to_string(Error::from_raw(ErrorDomain::Osal, 1, operation));
        const auto marker = "[operation=" + std::to_string(operation) + "]";
        if (formatted.find(marker) != std::string::npos) {
            return 1;
        }
    }

    // NTSTATUS must not accidentally run through the Win32 message table.
    constexpr std::int64_t access_denied_status = 0xC0000022;
    const Error status{osal::ErrorCode::SystemError,
                       osal::ErrorOperation::RandomBytes,
                       {NativeErrorSource::NtStatus, access_denied_status}};
    const auto status_text = error_to_string(status);
    if (status_text.find("native=3:3221225506]") == std::string::npos) {
        return 1;
    }

    const Error foreign{osal::ErrorCode::SystemError,
                        osal::ErrorOperation::ResolveAddress,
                        {static_cast<NativeErrorSource>(987), -123}};
    if (error_to_string(foreign).find("native=987:-123]") == std::string::npos) {
        return 1;
    }
    return 0;
}

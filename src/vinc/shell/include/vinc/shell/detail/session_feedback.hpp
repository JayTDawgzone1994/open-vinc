#pragma once

#include <vinc/shell/detail/tcl_runtime.hpp>
#include <optional>

namespace vinc::shell {
// Owned and accessed by the local Shell loop or one Socket Session's network loop.
// No worker writes this state. Successful responses never erase the last failure.
class SessionFeedback {
    std::optional<Error> failure_;
    std::string context_;
    std::string script_details_;
    bool recorded_ = false;

public:
    std::string failure(const Error& error, std::string_view target = {});
    std::string command(const Result<CommandReply>& result, std::string_view target = {});
    std::string script(const ScriptReport& report);
    std::string details() const;
};
}

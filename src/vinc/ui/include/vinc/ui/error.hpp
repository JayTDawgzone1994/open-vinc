#pragma once
#include <vinc/result.hpp>
namespace vinc::ui {
enum class ErrorCode : std::int32_t {
    InvalidArgument = 1,
    FileFailed = 2,
    InvalidPackage = 3,
    CapacityExceeded = 4,
    BindFailed = 5,
    WorkerFailed = 6,
    BusFailed = 7,
    SessionFailed = 8
};
enum class ErrorOperation : std::int32_t {
    Load = 1,
    Start = 2,
    Command = 3,
    InspectTopics = 4,
    Preview = 5,
    Session = 6
};
}
namespace vinc {
template <> struct ErrorCodeTraits<ui::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::Ui;
};
template <> struct ErrorOperationTraits<ui::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::Ui;
};
}

#pragma once
#include <vinc/result.hpp>

namespace vinc::output_hub {
enum class ErrorCode : std::int32_t {
    InvalidArgument = 1,
    ResourceExhausted = 2,
    Timeout = 3,
    ReadFailed = 4,
    SpawnFailed = 5,
    ServiceFailed = 6,
    WorkerFailed = 7
};
enum class ErrorOperation : std::int32_t {
    Subscribe = 1,
    Spawn = 2,
    ReadOutput = 3,
    Wait = 4,
    Create = 5,
    StartWorker = 6,
    MonitorOutput = 7
};
}

namespace vinc {
template <> struct ErrorCodeTraits<output_hub::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::OutputHub;
};

template <> struct ErrorOperationTraits<output_hub::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::OutputHub;
};
}

#pragma once
#include <vinc/result.hpp>

namespace vinc::shell {
// Published identifiers are explicit and independent of build order.
enum class ErrorCode : std::int32_t {
    InvalidArgument = 1,
    LineTooLong = 2,
    TooManyArguments = 3,
    InvalidCharacter = 4,
    UnterminatedQuote = 5,
    UnknownCommand = 6,
    SessionRequired = 7,
    Busy = 8,
    Closing = 9,
    Cancelled = 10,
    CommandFailed = 11,
    StartFailed = 12,
    CompletionFailed = 13,
    ReservationFailed = 14,
    OutputUnavailable = 15,
    TerminalFailed = 16,
    SocketFailed = 17,
    AlreadyStarted = 18,
    TclNotInitialized = 19,
    TclResourcesMissing = 20,
    TclInitializationFailed = 21,
    ScriptFailed = 22,
    WorkerFailed = 23,
    ResourceExhausted = 24,
    UnexpectedException = 25,
    ResumeRejected = 26,
    ReplyTooLarge = 27,
    Timeout = 28,
    TimeoutOutcomeUnknown = 29,
    CancelledOutcomeUnknown = 30,
    OutcomeUnknown = 31
};

enum class ErrorOperation : std::int32_t {
    Create = 1,
    InitializeTcl = 2,
    CreateInterpreter = 3,
    ReserveCommands = 4,
    SubscribeOutput = 5,
    ParseLine = 6,
    Execute = 7,
    StartModule = 8,
    Complete = 9,
    StartScript = 10,
    RunScript = 11,
    InstallCommands = 12,
    InstallArguments = 13,
    StartSocket = 14,
    OpenSocket = 15,
    Bind = 16,
    Listen = 17,
    GetEndpoint = 18,
    CreatePoller = 19,
    RegisterSocket = 20,
    Poll = 21,
    GenerateToken = 22,
    StartWorker = 23,
    OpenTerminal = 24,
    ReadTerminal = 25,
    Run = 26,
    WaitShutdown = 27,
    Resume = 28,
    Send = 29,
    Receive = 30,
    CloseSocket = 31,
    Accept = 32
};
}

namespace vinc {
template <> struct ErrorCodeTraits<shell::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::Shell;
};

template <> struct ErrorOperationTraits<shell::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::Shell;
};
}

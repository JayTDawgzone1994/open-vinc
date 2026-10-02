#pragma once

// Common OSAL result type: portable error category plus operation and native cause.
// Keep the native error when reporting failures; a portable category alone loses diagnostics.

#include <chrono>
#include <expected>
#include <system_error>

namespace vinc::osal {
using Deadline = std::chrono::steady_clock::time_point;
enum class ErrorCode {
    InvalidArgument,
    NotFound,
    PermissionDenied,
    ResourceExhausted,
    Unsupported,
    Timeout,
    Cancelled,
    ServiceClosed,
    Busy,
    ObservationLost,
    SystemError,
    Closed,
    PeerClosed,
    WouldBlock,
    AddressInUse,
    AddressNotAvailable,
    ConnectionRefused,
    ConnectionReset,
    NotConnected,
    MessageTooLarge,
    NetworkUnreachable,
    AlreadyExists
};
enum class Operation {
    InitializeService,
    Spawn,
    Observe,
    Wait,
    Terminate,
    Forget,
    Shutdown,
    CreatePipe,
    AdoptPipe,
    ReadPipe,
    WritePipe,
    CancelIo,
    WaitIo,
    CreateSocket,
    ParseAddress,
    ResolveAddress,
    BindSocket,
    ListenSocket,
    AcceptSocket,
    ConnectSocket,
    SendSocket,
    ReceiveSocket,
    ShutdownSocket,
    CloseSocket,
    QuerySocket,
    SetSocketOption,
    GetSocketOption,
    RegisterSocket,
    WaitSocket,
    WakeSocket,
    CreateSharedMemory,
    OpenSharedMemory,
    MapSharedMemory,
    UnmapSharedMemory,
    CloseSharedMemory,
    UnlinkSharedMemory,
    OpenTerminal,
    ReadTerminal,
    RandomBytes,
    CreateFileLock,
    OpenFileLock,
    LockFileLock,
    UnlockFileLock
};

struct OsError {
    ErrorCode code;
    Operation operation;
    std::error_code native{};
};
template <class T> using Result = std::expected<T, OsError>;
} // namespace vinc::osal

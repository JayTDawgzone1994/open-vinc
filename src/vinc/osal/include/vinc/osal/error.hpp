#pragma once
#include <system_error>

#include <vinc/result.hpp>

namespace vinc::osal {
// Stable local identifiers. The domain is part of every error's identity.
enum class ErrorCode : std::int32_t {
    InvalidArgument = 1,
    NotFound = 2,
    PermissionDenied = 3,
    ResourceExhausted = 4,
    Unsupported = 5,
    Timeout = 6,
    Cancelled = 7,
    ServiceClosed = 8,
    Busy = 9,
    ObservationLost = 10,
    SystemError = 11,
    Closed = 12,
    PeerClosed = 13,
    WouldBlock = 14,
    AddressInUse = 15,
    AddressNotAvailable = 16,
    ConnectionRefused = 17,
    ConnectionReset = 18,
    NotConnected = 19,
    MessageTooLarge = 20,
    NetworkUnreachable = 21,
    AlreadyExists = 22,
    InvalidEncoding = 23
};

enum class ErrorOperation : std::int32_t {
    Unspecified = 0,
    InitializeService = 1,
    Spawn = 2,
    Observe = 3,
    Wait = 4,
    Terminate = 5,
    Forget = 6,
    Shutdown = 7,
    CreatePipe = 8,
    AdoptPipe = 9,
    ReadPipe = 10,
    WritePipe = 11,
    CancelIo = 12,
    WaitIo = 13,
    CreateSocket = 14,
    ParseAddress = 15,
    ResolveAddress = 16,
    BindSocket = 17,
    ListenSocket = 18,
    AcceptSocket = 19,
    ConnectSocket = 20,
    SendSocket = 21,
    ReceiveSocket = 22,
    ShutdownSocket = 23,
    CloseSocket = 24,
    QuerySocket = 25,
    SetSocketOption = 26,
    GetSocketOption = 27,
    RegisterSocket = 28,
    WaitSocket = 29,
    WakeSocket = 30,
    CreateSharedMemory = 31,
    OpenSharedMemory = 32,
    MapSharedMemory = 33,
    UnmapSharedMemory = 34,
    CloseSharedMemory = 35,
    UnlinkSharedMemory = 36,
    OpenTerminal = 37,
    ReadTerminal = 38,
    RandomBytes = 39,
    CreateFileLock = 40,
    OpenFileLock = 41,
    LockFileLock = 42,
    UnlockFileLock = 43,
    PathFromUtf8 = 44,
    PathToUtf8 = 45,
    OpenLibrary = 46,
    FindSymbol = 47,
    CloseLibrary = 48,
    CreateNotification = 49,
    OpenNotification = 50,
    Notify = 51,
    WaitNotification = 52,
    CloseNotification = 53,
    UnlinkNotification = 54,
    ExecutablePath = 55
};
}

namespace vinc {
template <> struct ErrorCodeTraits<osal::ErrorCode> {
    static constexpr ErrorDomain domain = ErrorDomain::Osal;
};

template <> struct ErrorOperationTraits<osal::ErrorOperation> {
    static constexpr ErrorDomain domain = ErrorDomain::Osal;
};
}

namespace vinc::osal {
// Convert at standard-library exception/filesystem boundaries; no category pointer escapes.
NativeError native_error(const std::error_code& error) noexcept;
}

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
#else
#include <sys/resource.h>
#endif

namespace control_cost {
struct Usage {
    double cpu_ms = 0;
    std::uint64_t rss_bytes = 0;
    std::uint64_t peak_rss_bytes = 0;
    std::uint64_t threads = 0;
    std::uint64_t handles_or_fds = 0;
};

inline Usage usage() {
    Usage result;
#ifdef _WIN32
    const auto process = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    // Win32 ABI boundary: EX starts with the documented base counters layout;
    // cb supplies the actual writable structure size to this native API.
    if (!GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        throw std::runtime_error("GetProcessMemoryInfo failed");
    }
    result.rss_bytes = memory.WorkingSetSize;
    result.peak_rss_bytes = memory.PeakWorkingSetSize;
    DWORD handles = 0;
    if (!GetProcessHandleCount(process, &handles)) {
        throw std::runtime_error("GetProcessHandleCount failed");
    }
    result.handles_or_fds = handles;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("thread snapshot failed");
    }
    THREADENTRY32 thread{};
    thread.dwSize = sizeof(thread);
    if (!Thread32First(snapshot, &thread)) {
        CloseHandle(snapshot);
        throw std::runtime_error("thread enumeration failed");
    }
    do {
        if (thread.th32OwnerProcessID == GetCurrentProcessId()) {
            ++result.threads;
        }
    } while (Thread32Next(snapshot, &thread));
    const auto last_error = GetLastError();
    CloseHandle(snapshot);
    if (last_error != ERROR_NO_MORE_FILES) {
        throw std::runtime_error("thread enumeration incomplete");
    }
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        throw std::runtime_error("GetProcessTimes failed");
    }
    const auto ticks = [](FILETIME value) {
        return (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    result.cpu_ms = double(ticks(kernel) + ticks(user)) / 10000.0;
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            status >> result.rss_bytes;
            result.rss_bytes *= 1024;
        } else if (key == "Threads:") {
            status >> result.threads;
        }
        std::string rest;
        std::getline(status, rest);
    }
    if (!status.eof() || !result.threads) {
        throw std::runtime_error("process status unavailable");
    }
    status.close();
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++result.handles_or_fds;
    }
    // Enumeration itself opens one FD, already closed after the loop.
    --result.handles_or_fds;
    rusage counters{};
    if (getrusage(RUSAGE_SELF, &counters) != 0) {
        throw std::runtime_error("getrusage failed");
    }
    result.peak_rss_bytes = std::uint64_t(counters.ru_maxrss) * 1024;
    result.cpu_ms = double(counters.ru_utime.tv_sec + counters.ru_stime.tv_sec) * 1000.0 +
        double(counters.ru_utime.tv_usec + counters.ru_stime.tv_usec) / 1000.0;
#elif defined(__APPLE__)
    const auto pid = getpid();
    struct proc_taskinfo task{};
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task)) != sizeof(task)) {
        throw std::runtime_error("proc_pidinfo task sample unavailable");
    }
    result.rss_bytes = task.pti_resident_size;
    result.threads = task.pti_threadnum;

    const auto estimated_bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (estimated_bytes <= 0) {
        throw std::runtime_error("proc_pidinfo FD size unavailable");
    }
    // The descriptor table may grow between the two calls.
    constexpr std::size_t descriptor_headroom = 32;
    std::vector<struct proc_fdinfo> descriptors(
        std::size_t(estimated_bytes) / sizeof(struct proc_fdinfo) + descriptor_headroom);
    const auto capacity_bytes = int(descriptors.size() * sizeof(struct proc_fdinfo));
    const auto listed_bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, descriptors.data(), capacity_bytes);
    if (listed_bytes <= 0 || listed_bytes >= capacity_bytes ||
        listed_bytes % sizeof(struct proc_fdinfo) != 0) {
        throw std::runtime_error("proc_pidinfo FD sample incomplete");
    }
    result.handles_or_fds = std::size_t(listed_bytes) / sizeof(struct proc_fdinfo);

    rusage counters{};
    if (getrusage(RUSAGE_SELF, &counters) != 0) {
        throw std::runtime_error("getrusage failed");
    }
    // Darwin reports ru_maxrss in bytes.
    result.peak_rss_bytes = std::uint64_t(counters.ru_maxrss);
    result.cpu_ms = double(counters.ru_utime.tv_sec + counters.ru_stime.tv_sec) * 1000.0 +
        double(counters.ru_utime.tv_usec + counters.ru_stime.tv_usec) / 1000.0;
#else
    throw std::runtime_error("cost resource sampler needs native platform implementation");
#endif
    return result;
}
}

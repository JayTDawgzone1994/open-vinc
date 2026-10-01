#pragma once
#include <string_view>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

// Test-owned stdin redirection, confined to this CTest process.
class InputPipe {
#ifdef _WIN32
    HANDLE original_ = GetStdHandle(STD_INPUT_HANDLE), reader_ = INVALID_HANDLE_VALUE, writer_ = INVALID_HANDLE_VALUE;
#else
    int original_ = -1, reader_ = -1, writer_ = -1;
#endif
public:
    InputPipe() {
#ifdef _WIN32
        if (!CreatePipe(&reader_, &writer_, nullptr, 0)) throw std::runtime_error("CreatePipe");
        if (!SetStdHandle(STD_INPUT_HANDLE, reader_)) throw std::runtime_error("SetStdHandle");
#else
        int fds[2]; if (pipe(fds)) throw std::runtime_error("pipe");
        original_ = dup(STDIN_FILENO); reader_ = fds[0]; writer_ = fds[1];
        if (original_ < 0 || dup2(reader_, STDIN_FILENO) < 0) throw std::runtime_error("dup stdin");
#endif
    }
    ~InputPipe() {
        end();
#ifdef _WIN32
        SetStdHandle(STD_INPUT_HANDLE, original_); CloseHandle(reader_);
#else
        dup2(original_, STDIN_FILENO); close(original_); close(reader_);
#endif
    }
    void send(std::string_view bytes) {
        while (!bytes.empty()) {
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(writer_, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) throw std::runtime_error("write stdin");
#else
            const auto count = write(writer_, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::runtime_error("write stdin");
#endif
            if (!count) throw std::runtime_error("zero write");
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
    }
    void end() noexcept {
#ifdef _WIN32
        if (writer_ != INVALID_HANDLE_VALUE) CloseHandle(writer_);
        writer_ = INVALID_HANDLE_VALUE;
#else
        if (writer_ >= 0) close(writer_);
        writer_ = -1;
#endif
    }
};

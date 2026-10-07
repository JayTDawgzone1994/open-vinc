#pragma once

// RAII test input source: temporarily replace stdin and restore it on destruction.
// Tests can deliver control bytes or EOF without borrowing the user's console.

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

#ifdef VINC_SHELL_TESTING
namespace input_pipe_test {
void checkpoint();
}
#endif

// Test-owned stdin redirection, confined to this CTest process.
class InputPipe {
#ifdef _WIN32
    HANDLE original_ = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE reader_ = INVALID_HANDLE_VALUE;
    HANDLE writer_ = INVALID_HANDLE_VALUE;
#else
    int original_ = -1;
    int reader_ = -1;
    int writer_ = -1;
#endif
public:
    // No operation after installing stdin can fail. Roll back raw resources if
    // acquisition or installation fails before this object is fully constructed.
    InputPipe() {
        try {
            acquire();
        } catch (...) {
            release();
            throw;
        }
    }

private:
    void acquire() {
#ifdef _WIN32
        if (!CreatePipe(&reader_, &writer_, nullptr, 0)) {
            throw std::runtime_error("CreatePipe");
        }

#ifdef VINC_SHELL_TESTING
        input_pipe_test::checkpoint();
#endif
        if (!SetStdHandle(STD_INPUT_HANDLE, reader_)) {
            throw std::runtime_error("SetStdHandle");
        }
#else
        int descriptors[2];
        if (pipe(descriptors)) {
            throw std::runtime_error("pipe");
        }

        original_ = dup(STDIN_FILENO);
        reader_ = descriptors[0];
        writer_ = descriptors[1];
#ifdef VINC_SHELL_TESTING
        input_pipe_test::checkpoint();
#endif
        if (original_ < 0 || dup2(reader_, STDIN_FILENO) < 0) {
            throw std::runtime_error("dup stdin");
        }
#endif
    }

    void release() noexcept {
        end();
#ifdef _WIN32
        if (reader_ != INVALID_HANDLE_VALUE) {
            CloseHandle(reader_);
        }
        reader_ = INVALID_HANDLE_VALUE;
#else
        if (original_ >= 0) {
            close(original_);
        }
        if (reader_ >= 0) {
            close(reader_);
        }
        original_ = -1;
        reader_ = -1;
#endif
    }

public:
    InputPipe(const InputPipe&) = delete;
    InputPipe& operator=(const InputPipe&) = delete;

    ~InputPipe() {
#ifdef _WIN32
        SetStdHandle(STD_INPUT_HANDLE, original_);
#else
        dup2(original_, STDIN_FILENO);
#endif
        release();
    }

    void send(std::string_view bytes) {
        while (!bytes.empty()) {
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(writer_, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
                throw std::runtime_error("write stdin");
            }
#else
            const auto count = write(writer_, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }

            if (count < 0) {
                throw std::runtime_error("write stdin");
            }
#endif
            if (!count) {
                throw std::runtime_error("zero write");
            }

            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
    }

    void end() noexcept {
#ifdef _WIN32
        if (writer_ != INVALID_HANDLE_VALUE) {
            CloseHandle(writer_);
        }

        writer_ = INVALID_HANDLE_VALUE;
#else
        if (writer_ >= 0) {
            close(writer_);
        }

        writer_ = -1;
#endif
    }
};

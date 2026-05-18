#include <iostream>
#include <string>
#include <vector>

// --- 平台偵測層 ---
#if defined(_WIN32) || defined(_WIN64)
    #define HOLON_PLATFORM_WIN
    #include <conio.h>
    #include <windows.h>
#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    #define HOLON_PLATFORM_POSIX
    #include <termios.h>
    #include <unistd.h>
#else
    #define HOLON_PLATFORM_UNKNOWN
#endif

namespace holon {

class TerminalGuard {
public:
    TerminalGuard() 
    {
#ifdef HOLON_PLATFORM_WIN
        // Windows 不需要特別設定切換，但可以設定 UTF-8 輸出
        SetConsoleOutputCP(CP_UTF8);
#elif HOLON_PLATFORM_POSIX
// 檢查 STDIN 是否為 TTY
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
            struct termios new_termios = orig_termios;
            new_termios.c_lflag &= ~(ICANON | ECHO);
            // 建議同時關閉 ISIG，如果你想在迴圈裡自己處理 Ctrl+C
            // new_termios.c_lflag &= ~ISIG; 
            tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
            _is_a_tty = true; // 紀錄狀態，解構時才不會亂恢復
        }
    } else {
        // 運行在後台服務模式，跳過所有 TTY 設定
        _is_a_tty = false;
    }
#endif
    }

    ~TerminalGuard() 
    {
#ifdef HOLON_PLATFORM_POSIX
        // Linux: 恢復原始終端機狀態
        if (_is_a_tty) {
            tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        }
#endif
    }

    // 抽象出的即時字元讀取
    char get_char() 
    {
#ifdef HOLON_PLATFORM_WIN
        return static_cast<char>(_getch());
#else
        char ch;
        read(STDIN_FILENO, &ch, 1);
        return ch;
#endif
    }

private:
#ifndef HOLON_PLATFORM_WIN
    struct termios orig_termios;
#endif
};

} // namespace holon
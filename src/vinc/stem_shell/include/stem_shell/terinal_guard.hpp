#include <iostream>
#include <string>
#include <vector>

// --- 平台偵測層（完全無 #else / #elif） ---
#if defined(_WIN32) || defined(_WIN64)
    #define VINC_PLATFORM_WIN
    #include <conio.h>
    #include <windows.h>
#endif

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    #define VINC_PLATFORM_POSIX
    #include <termios.h>
    #include <unistd.h>
#endif

#ifndef VINC_PLATFORM_WIN
#ifndef VINC_PLATFORM_POSIX
    #define VINC_PLATFORM_UNKNOWN
#endif
#endif

namespace vinc {

class TerminalGuard {
public:
    TerminalGuard() 
    {
#if defined(VINC_PLATFORM_WIN)
        // Windows 環境設定
        SetConsoleOutputCP(CP_UTF8);
        _is_a_tty = true; 
#endif

#if defined(VINC_PLATFORM_POSIX)
        _is_a_tty = false; // 預設初始化
        if (isatty(STDIN_FILENO)) {
            if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
                struct termios new_termios = orig_termios;
                // 關閉標準輸入與回顯
                new_termios.c_lflag &= ~(ICANON | ECHO | ISIG);
                
                // 🎯 解決 Linux 沒反應的核心：明確設定控制字元
                new_termios.c_cc[VMIN] = 1;  // 只要讀到 1 個字元就立刻返回（等同 _getch）
                new_termios.c_cc[VTIME] = 0; // 停用超時等待
                
                tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
                _is_a_tty = true; 
            }
        }
#endif
    }

    ~TerminalGuard() 
    {
#if defined(VINC_PLATFORM_POSIX)
        // Linux: 恢復原始終端機狀態
        if (_is_a_tty) {
            tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        }
#endif
    }

    // 抽象出的即時字元讀取
    char get_char() 
    {
#if defined(VINC_PLATFORM_WIN)
        return static_cast<char>(_getch());
#endif

#if defined(VINC_PLATFORM_POSIX)
        char ch = 0;
        // 安全讀取單一字元
        if (read(STDIN_FILENO, &ch, 1) < 0) {
            return 0;
        }
        return ch;
#endif

#if defined(VINC_PLATFORM_UNKNOWN)
        return 0;
#endif
    }

private:
    // 🎯 修正點 2：成功創建並初始化成員變數
    bool _is_a_tty = false;

#if defined(VINC_PLATFORM_POSIX)
    struct termios orig_termios;
#endif
};

} // namespace vinc
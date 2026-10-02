#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!size) return 2;
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, text.data(), size, nullptr, nullptr)) return 2;
        text.pop_back(); args.push_back(std::move(text));
    }
    return application(args);
}
#else
int main(int argc, char** argv) { return application(std::vector<std::string>(argv, argv + argc)); }
#endif

int library_runtime();

int main() {
#if defined(_DLL)
    constexpr int expected_runtime = 1;
#else
    constexpr int expected_runtime = 0;
#endif
    if (expected_runtime != EXPECTED_DYNAMIC) {
        return 1;
    }
    if (library_runtime() != expected_runtime) {
        return 2;
    }
    return 0;
}

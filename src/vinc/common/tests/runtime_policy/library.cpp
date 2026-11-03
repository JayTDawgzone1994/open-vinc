int library_runtime() {
#if defined(_DLL)
    return 1;
#else
    return 0;
#endif
}

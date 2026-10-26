#pragma once

// Static Common copies in different images must retain their own bridge binding.
// ELF symbol interposition must not redirect a Plugin's fatal call to another image.
#if defined(__GNUC__) || defined(__clang__)
#define VINC_LOCAL __attribute__((visibility("hidden")))
#else
#define VINC_LOCAL
#endif

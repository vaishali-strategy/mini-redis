#pragma once
// A tiny test helper: CHECK(cond) records a failure and keeps going.

#include <cstdio>

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

inline int finish(const char* name) {
    if (g_failures == 0) {
        std::printf("%s: all %d checks passed\n", name, g_checks);
        return 0;
    }
    std::printf("%s: %d of %d checks FAILED\n", name, g_failures, g_checks);
    return 1;
}

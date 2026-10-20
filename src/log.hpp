#pragma once

#include <cstdarg>
#include <cstdio>

inline bool g_verbose = false;

inline void log_v(const char* fmt, va_list ap) {
    std::fputs("quizhint: ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

[[gnu::format(printf, 1, 2)]] inline void log_info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v(fmt, ap);
    va_end(ap);
}

// Подробности и замеры времени — только с ключом -v.
[[gnu::format(printf, 1, 2)]] inline void log_debug(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    log_v(fmt, ap);
    va_end(ap);
}

/* Name a host thread for profilers, debuggers and per-thread CPU reports
 * (Windows thread description). printf-style; no-op elsewhere. */
#ifndef PS3_THREAD_NAME_H
#define PS3_THREAD_NAME_H
#include <stdarg.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
static inline void ps3_thread_name(HANDLE thread, const char* format, ...)
{
    char name[64];
    va_list args;
    va_start(args, format);
    vsnprintf(name, sizeof name, format, args);
    va_end(args);
    wchar_t wide[64];
    if (thread && MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64) > 0)
        SetThreadDescription(thread, wide);
}
#else
static inline void ps3_thread_name(void* thread, const char* format, ...) { (void)thread; (void)format; }
#endif
#endif

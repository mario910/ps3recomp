/* Name a host thread for profilers, debuggers and per-thread CPU reports
 * (Windows thread description, pthread name elsewhere). printf-style. */
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
#include <string.h>
#include <wchar.h>
#include "win32_compat.h"
/* POSIX: pthread_setname_np through win32_compat. Linux/Android keep only 15
 * characters, so the long runtime names are abbreviated first ("SPURS pool
 * lane 3" -> "lane3", "SPURS chain 701EB500" -> "ch 701EB500"). */
static inline void ps3_thread_name(void* thread, const char* format, ...)
{
    static const char* const abbreviations[][2] = {
        {"SPURS pool lane ", "lane"}, {"SPURS chain worker ", "cw"},
        {"SPURS chain ", "ch "}, {"cellAudio mixer", "audio mixer"},
        {"SPU workload", "spu workload"}, {"PPU ", "P "},
    };
    char name[64], short_name[64];
    va_list args;
    va_start(args, format);
    vsnprintf(name, sizeof name, format, args);
    va_end(args);
    snprintf(short_name, sizeof short_name, "%s", name);
    for (size_t i = 0; i < sizeof abbreviations / sizeof abbreviations[0]; ++i) {
        size_t n = strlen(abbreviations[i][0]);
        if (!strncmp(name, abbreviations[i][0], n)) {
            snprintf(short_name, sizeof short_name, "%s%s", abbreviations[i][1], name + n);
            break;
        }
    }
    wchar_t wide[64];
    if (thread && mbstowcs(wide, short_name, 64) != (size_t)-1)
        SetThreadDescription(thread, wide);
}
#endif
#endif

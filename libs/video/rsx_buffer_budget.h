#pragma once
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
/* Bounded configuration, not a request to grow GPU upload buffers endlessly. */
static inline uint32_t rsx_parse_budget(const char* text, uint32_t fallback,
                                       uint32_t minimum, uint32_t maximum)
{
    if (!text || !*text || *text == '-') return fallback;
    char* end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || *end || value < minimum || value > maximum)
        return fallback;
    return (uint32_t)value;
}

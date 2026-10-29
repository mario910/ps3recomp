#ifndef PS3_SPURS_ENV_CACHE_H
#define PS3_SPURS_ENV_CACHE_H
#include <stdlib.h>

/* Startup-only settings: names must have stable addresses (string literals),
 * and the environment must not change while workers run. Values stay CRT-owned.
 * Each thread owns its cache: a compiler barrier around shared pointer stores
 * would not make concurrent readers/writers safe under the C memory model. */
#if defined(__cplusplus)
#define SPURS_ENV_TLS thread_local
#elif defined(_MSC_VER)
#define SPURS_ENV_TLS __declspec(thread)
#else
#define SPURS_ENV_TLS _Thread_local
#endif

static inline const char* spurs_cached_env(const char* name)
{
    static SPURS_ENV_TLS struct {
        const char* keys[32];
        const char* values[32];
        unsigned used;
    } cache;
    for (unsigned i = 0; i < cache.used; ++i)
        if (cache.keys[i] == name) return cache.values[i];
    const char* value = getenv(name);
    if (cache.used < 32) {
        cache.keys[cache.used] = name;
        cache.values[cache.used++] = value; /* NULL is a cached miss too. */
    }
    /* A full cache still returns the correct value, without counter overflow. */
    return value;
}
#undef SPURS_ENV_TLS
#endif

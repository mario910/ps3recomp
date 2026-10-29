#ifndef PS3_RSX_ENV_CACHE_H
#define PS3_RSX_ENV_CACHE_H
#include <stdint.h>
#include <stdlib.h>
/* Renderer-local STARTUP settings only. Keys must be immutable string
 * literals; values remain CRT-owned, so no environment mutation is allowed
 * while this opt-in cache is in use. File-based live probes stay live. */
typedef struct {
    struct { const char* key; char* value; } slots[512];
} RsxEnvCache;
static inline char* rsx_cached_env(RsxEnvCache* cache, const char* name)
{
    uintptr_t hash = (uintptr_t)name;
    hash ^= hash >> 17;
    unsigned slot = (unsigned)(hash >> 3) & 511u;
    if (cache->slots[slot].key == name) return cache->slots[slot].value;
    char* value = getenv(name);
    cache->slots[slot].key = name;
    cache->slots[slot].value = value; /* NULL is a cached miss too */
    return value;
}
#endif

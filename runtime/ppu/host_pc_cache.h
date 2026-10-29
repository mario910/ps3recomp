#pragma once
#include <stdint.h>

/* Small per-thread memo for an immutable host-PC -> guest-symbol resolver.
 * Exact full-width keys, including zero; collisions replace, never alias.
 * This is diagnostic attribution only, not the guest call dispatcher. */
template<unsigned Capacity> struct HostPcCache {
    static_assert(Capacity && !(Capacity & (Capacity - 1)));
    struct Entry { uintptr_t host; uint32_t guest; bool valid; } entries[Capacity]{};
    static unsigned bucket(uintptr_t host) {
        return (unsigned)((host >> 4) ^ (host >> 12)) & (Capacity - 1);
    }
    bool get(uintptr_t host, uint32_t& guest) const {
        const Entry& entry = entries[bucket(host)];
        if (!entry.valid || entry.host != host) return false;
        guest = entry.guest;
        return true;
    }
    void put(uintptr_t host, uint32_t guest) {
        entries[bucket(host)] = { host, guest, true };
    }
};

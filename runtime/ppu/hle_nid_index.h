#pragma once
#include <stdint.h>

/* Registration-time index. Slot values are table index + 1, so NID zero and
 * 0xffffffff remain valid keys. Duplicate registration keeps the first entry,
 * matching the original forward linear scan. Populate before guest threads. */
template<unsigned Capacity> struct HleNidIndex {
    static_assert(Capacity && !(Capacity & (Capacity - 1)));
    uint32_t keys[Capacity]{};
    uint32_t values[Capacity]{};
    static unsigned bucket(uint32_t nid) {
        nid ^= nid >> 16;
        return (nid * 2654435761u) & (Capacity - 1);
    }
    bool insert(uint32_t nid, uint32_t index) {
        for (unsigned i = bucket(nid), n = 0; n < Capacity;
             ++n, i = (i + 1) & (Capacity - 1)) {
            if (!values[i]) { keys[i] = nid; values[i] = index + 1; return true; }
            if (keys[i] == nid) return true;
        }
        return false;
    }
    int find(uint32_t nid) const {
        for (unsigned i = bucket(nid), n = 0; n < Capacity;
             ++n, i = (i + 1) & (Capacity - 1)) {
            if (!values[i]) return -1;
            if (keys[i] == nid) return (int)(values[i] - 1);
        }
        return -1;
    }
};

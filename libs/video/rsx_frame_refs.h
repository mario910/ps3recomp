#ifndef RSX_FRAME_REFS_H
#define RSX_FRAME_REFS_H
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
static inline int rsx_fence_reached(uint64_t completed, uint64_t target) {
    /* D3D12 uses UINT64_MAX to report device removal, not completion. */
    return completed != UINT64_MAX && completed >= target;
}
/* One owned reference per recorded draw. Cache eviction must not invalidate
 * commands already recorded, nor force a second lookup against a moved cache.
 * Only drain after the caller has positively observed GPU completion. */
typedef struct { void** slots; size_t capacity, used; } rsx_frame_refs;
static inline int rsx_frame_refs_init(rsx_frame_refs* refs, size_t capacity) {
    if (refs->slots) return refs->capacity == capacity;
    refs->slots = (void**)calloc(capacity, sizeof(void*));
    if (!refs->slots) return 0;
    refs->capacity = capacity;
    return 1;
}
static inline int rsx_frame_refs_pin(rsx_frame_refs* refs, size_t slot,
                                    void* object, void (*retain)(void*)) {
    if (slot >= refs->capacity || refs->slots[slot]) return 0;
    if (object) retain(object);
    refs->slots[slot] = object;
    if (refs->used <= slot) refs->used = slot + 1;
    return 1;
}
static inline void* rsx_frame_refs_get(const rsx_frame_refs* refs, size_t slot) {
    return slot < refs->used ? refs->slots[slot] : NULL;
}
static inline int rsx_frame_refs_drain(rsx_frame_refs* refs, int gpu_complete,
                                      void (*release)(void*)) {
    if (!gpu_complete) return 0;
    for (size_t i=0; i<refs->used; ++i) {
        if (refs->slots[i]) release(refs->slots[i]);
        refs->slots[i] = NULL;
    }
    refs->used = 0;
    return 1;
}
#endif

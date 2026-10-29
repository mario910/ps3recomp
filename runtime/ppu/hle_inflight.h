#pragma once
#include <atomic>

/* Independent hot guest threads must not invalidate one cache line merely
 * to update watchdog metadata. Readers are rare, concurrent and diagnostic. */
template<unsigned Count> struct HleInflightSlots {
    struct alignas(64) Slot { std::atomic<const char*> name{nullptr}; } slots[Count];
    static_assert(sizeof(Slot) == 64);
    const char* get(unsigned thread) const {
        return thread < Count ? slots[thread].name.load(std::memory_order_relaxed) : nullptr;
    }
    void set(unsigned thread, const char* name) {
        if (thread < Count) slots[thread].name.store(name, std::memory_order_relaxed);
    }
};

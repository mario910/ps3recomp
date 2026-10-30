#include "spurs_command_wait.h"
#include "../platform/win32_compat.h"
#include <string.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif

enum { WAITERS = 32 };
typedef struct { uint32_t pc, chain; int active; CONDITION_VARIABLE wake; } Waiter;
static Waiter waiters[WAITERS];
static SRWLOCK lock = SRWLOCK_INIT;
uint64_t g_spurs_command_wait_lines;

static uint64_t read_be64(const uint8_t* memory, uint32_t address)
{
    uint64_t value;
    memcpy(&value, memory + address, sizeof value);
    return __builtin_bswap64(value);
}

void spurs_command_written(uint32_t address)
{
    AcquireSRWLockExclusive(&lock);
    for (unsigned i = 0; i < WAITERS; ++i) {
        Waiter* w = &waiters[i];
        if (w->active && (w->pc == address || w->chain == address))
            WakeConditionVariable(&w->wake);
    }
    ReleaseSRWLockExclusive(&lock);
}

/* A geometry job publishes the next JOBLIST of the other chain with a plain
 * DMA PUT, which never reaches the PPU store hook. Unannounced, the parked
 * walker sleeps out its bound and resumes on the next host timer tick. The
 * fence orders the copy before the filter load, pairing with the waiter's
 * RMW registration; a miss still only falls back to that bound. */
void spurs_command_dma_written(uint32_t ea, uint32_t size)
{
    if (!size) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const uint64_t lines = __atomic_load_n(&g_spurs_command_wait_lines, __ATOMIC_ACQUIRE);
    if (!lines) return;
    const uint64_t lo = ea, hi = (uint64_t)ea + size;
    for (uint64_t a = lo & ~127ull; a < hi; a += 128u) {
        if (!(lines & spurs_command_line_bit((uint32_t)a))) continue;
        AcquireSRWLockExclusive(&lock);
        for (unsigned i = 0; i < WAITERS; ++i) {
            Waiter* w = &waiters[i];
            if (w->active && ((w->pc < hi && w->pc + 8ull > lo) ||
                              (w->chain < hi && w->chain + 8ull > lo)))
                WakeConditionVariable(&w->wake);
        }
        ReleaseSRWLockExclusive(&lock);
        return;
    }
}

static unsigned logical_processors(void)
{
#ifdef _WIN32
    return (unsigned)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1u;
#endif
}

/* Poll the slot for up to SPURS_JTS_SPIN_US us before parking, as pool lanes
 * do (SPURS_POOL_SPIN_US, same automatic default: 1000 with 12+ logical
 * processors, else off; a set value is used as given). A publication usually
 * lands within one stage of the other chain, sooner than an OS wake-up.
 * Guest memory is read with atomic loads so the poll cannot be hoisted; the
 * caller re-reads the slot either way. */
static uint64_t spin_ticks(void)
{
    static uint64_t ticks = UINT64_MAX;
    uint64_t t = __atomic_load_n(&ticks, __ATOMIC_RELAXED);
    if (t == UINT64_MAX) {
        const char* e = getenv("SPURS_JTS_SPIN_US");
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        const long us = !e || !*e ? (logical_processors() >= 12 ? 1000 : 0) : atol(e);
        t = us > 0 && us <= 100000 ? (uint64_t)f.QuadPart * (uint64_t)us / 1000000u : 0;
        __atomic_store_n(&ticks, t, __ATOMIC_RELAXED);
    }
    return t;
}

static int published(const uint8_t* memory, uint32_t pc, uint32_t chain, uint64_t published_pc)
{
    return __builtin_bswap64(__atomic_load_n((const uint64_t*)(memory + pc), __ATOMIC_ACQUIRE)) != 0x800000012ull ||
           __builtin_bswap64(__atomic_load_n((const uint64_t*)(memory + chain), __ATOMIC_ACQUIRE)) != published_pc;
}

void spurs_command_wait(const uint8_t* memory, uint32_t pc, uint32_t chain,
                        uint64_t published_pc, unsigned timeout_ms)
{
    const uint64_t ticks = !((pc | chain) & 7) ? spin_ticks() : 0;
    if (ticks) {
        LARGE_INTEGER start, now;
        QueryPerformanceCounter(&start);
        for (;;) {
            for (int n = 0; n < 32; ++n) {
                if (published(memory, pc, chain, published_pc)) return;
                YieldProcessor();
            }
            QueryPerformanceCounter(&now);
            if ((uint64_t)(now.QuadPart - start.QuadPart) >= ticks) break;
        }
    }
    AcquireSRWLockExclusive(&lock);
    unsigned i = 0;
    for (; i < WAITERS && waiters[i].active; ++i) {}
    if (i == WAITERS) {
        ReleaseSRWLockExclusive(&lock);
        Sleep(timeout_ms);
        return;
    }
    Waiter* w = &waiters[i];
    w->pc = pc; w->chain = chain; w->active = 1;
    __atomic_fetch_or(&g_spurs_command_wait_lines,
                      spurs_command_line_bit(pc) | spurs_command_line_bit(chain), __ATOMIC_RELEASE);
    /* A write before registration is observed here; one after registration
     * takes this lock to notify, so cannot slip between check and park.
     * Native DMA/uninstrumented writes still have the bounded timeout. */
    if (read_be64(memory, pc) == 0x800000012ull && read_be64(memory, chain) == published_pc)
        SleepConditionVariableSRW(&w->wake, &lock, timeout_ms, 0);
    w->active = 0;
    uint64_t lines = 0;
    for (unsigned n = 0; n < WAITERS; ++n)
        if (waiters[n].active)
            lines |= spurs_command_line_bit(waiters[n].pc) | spurs_command_line_bit(waiters[n].chain);
    __atomic_store_n(&g_spurs_command_wait_lines, lines, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&lock);
}

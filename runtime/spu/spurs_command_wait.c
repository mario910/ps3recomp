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
/* Writers change the occupied slots under lock; DMA readers take a snapshot
 * after observing the published line mask. */
static uint32_t occupied_slots;
static SRWLOCK lock = SRWLOCK_INIT;
uint64_t g_spurs_command_wait_lines;

extern int spurs_job_idle_step(void);

static uint64_t read_be64(const uint8_t* memory, uint32_t address)
{
    uint64_t value;
    memcpy(&value, memory + address, sizeof value);
    return __builtin_bswap64(value);
}

void spurs_command_written(uint32_t address)
{
    AcquireSRWLockExclusive(&lock);
    for (uint32_t slots = __atomic_load_n(&occupied_slots, __ATOMIC_RELAXED);
         slots; slots &= slots - 1) {
        Waiter* w = &waiters[__builtin_ctz(slots)];
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
    const uint64_t lo = ea, hi = (uint64_t)ea + size;
    /* Unfenced pre-check (SPURS_DMA_NOTIFY_FENCE=1 restores the fence on every
     * PUT). Nearly every PUT is vertex or media data that no walker waits
     * for, and the store-load fence cost more than the rest of this function
     * on each of them. Without it, a walker that registers in the few
     * nanoseconds between this PUT's copy and the load below can be missed;
     * it then re-reads its slot after one park step (SPURS_JTS_PARK_US), the
     * same bound that already covers unhooked publications. A PUT that does
     * hit a registered line is fenced and re-checked as before. */
    static int always_fence = -1;
    if (__builtin_expect(always_fence < 0, 0)) {
        const char* e = getenv("SPURS_DMA_NOTIFY_FENCE");
        always_fence = !(e && e[0] == '0');   /* default: fence every PUT; =0 measured 17 vs 37 FPS (PPU word-wait relies on this fence) */
    }
    if (!always_fence) {
        const uint64_t hint = __atomic_load_n(&g_spurs_command_wait_lines, __ATOMIC_RELAXED);
        if (!hint) return;
        if (hi - (lo & ~127ull) <= 256) {
            int maybe = 0;
            for (uint64_t a = lo & ~127ull; a < hi && !maybe; a += 128u)
                maybe = (hint & spurs_command_line_bit((uint32_t)a)) != 0;
            if (!maybe) return;
        }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const uint64_t lines = __atomic_load_n(&g_spurs_command_wait_lines, __ATOMIC_ACQUIRE);
    if (!lines) return;
    // For long transfers inspect the few occupied waiter slots directly.
    // Keep the mandatory post-copy fence and repeat the overlap under lock.
    if (hi - (lo & ~127ull) <= 256 &&
        !(lines & (spurs_command_line_bit((uint32_t)lo) | spurs_command_line_bit((uint32_t)(hi-1))))) return;
    int overlap = 0;
    for (uint32_t slots = __atomic_load_n(&occupied_slots, __ATOMIC_ACQUIRE);
         slots && !overlap; slots &= slots - 1) {
        Waiter* w = &waiters[__builtin_ctz(slots)];
        if (!__atomic_load_n(&w->active, __ATOMIC_RELAXED)) continue;
        const uint64_t pc = __atomic_load_n(&w->pc, __ATOMIC_RELAXED);
        const uint64_t chain = __atomic_load_n(&w->chain, __ATOMIC_RELAXED);
        overlap = (pc < hi && pc + 8ull > lo) || (chain < hi && chain + 8ull > lo);
    }
    if (!overlap) return;
    AcquireSRWLockExclusive(&lock);
    for (uint32_t slots = __atomic_load_n(&occupied_slots, __ATOMIC_RELAXED);
         slots; slots &= slots - 1) {
        Waiter* w = &waiters[__builtin_ctz(slots)];
        if (w->active && ((w->pc < hi && w->pc + 8ull > lo) ||
                          (w->chain < hi && w->chain + 8ull > lo)))
            WakeConditionVariable(&w->wake);
    }
    ReleaseSRWLockExclusive(&lock);
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

/* SPURS_JTS_PARK_US (default 200; 0 = the caller's millisecond bound): how
 * long a parked walker sleeps before re-reading its slot. Notifications cover
 * PPU std stores and instrumented SPU PUTs, but some publications arrive by
 * other paths (native host jobs, unhooked DMA), and those were seen only when
 * the 1 ms bound expired -- several times a frame on the critical chain. */
static unsigned park_us(void)
{
    static int us = -1;
    int v = __atomic_load_n(&us, __ATOMIC_RELAXED);
    if (v < 0) {
        const char* e = getenv("SPURS_JTS_PARK_US");
        v = e && *e ? atoi(e) : 200;
        if (v < 0 || v > 1000000) v = 200;
        __atomic_store_n(&us, v, __ATOMIC_RELAXED);
    }
    return (unsigned)v;
}

#ifndef _WIN32
/* SleepConditionVariableSRW with a microsecond bound (the shim's is in ms). */
static void sleep_srw_us(CONDITION_VARIABLE* c, SRWLOCK* l, unsigned us)
{
    ps3_condvar* cv = ps3_cv(c);
    if (!cv) { ReleaseSRWLockExclusive(l); usleep(us); AcquireSRWLockExclusive(l); return; }
    pthread_mutex_lock(&cv->m);
    const unsigned g = cv->gen;
    ReleaseSRWLockExclusive(l);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)(us % 1000000u) * 1000L;
    ts.tv_sec += (time_t)(us / 1000000u);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int rc = 0;
    while (cv->gen == g && rc == 0) rc = pthread_cond_timedwait(&cv->c, &cv->m, &ts);
    pthread_mutex_unlock(&cv->m);
    AcquireSRWLockExclusive(l);
}
#endif

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
    /* About to park: do this thread's idle job-staging work first, one chunk
     * at a time, rechecking the slot in between (spurs_job.c). */
    while (spurs_job_idle_step())
        if (published(memory, pc, chain, published_pc)) return;
    if (published(memory, pc, chain, published_pc)) return;
    AcquireSRWLockExclusive(&lock);
    const uint32_t slots = __atomic_load_n(&occupied_slots, __ATOMIC_RELAXED);
    if (slots == UINT32_MAX) {
        ReleaseSRWLockExclusive(&lock);
        Sleep(timeout_ms);
        return;
    }
    const unsigned i = __builtin_ctz(~slots);
    Waiter* w = &waiters[i];
    __atomic_store_n(&w->pc, pc, __ATOMIC_RELAXED); __atomic_store_n(&w->chain, chain, __ATOMIC_RELAXED);
    __atomic_store_n(&w->active, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&occupied_slots, slots | (1u << i), __ATOMIC_RELEASE);
    __atomic_fetch_or(&g_spurs_command_wait_lines,
                      spurs_command_line_bit(pc) | spurs_command_line_bit(chain), __ATOMIC_RELEASE);
    /* A write before registration is observed here; one after registration
     * takes this lock to notify, so cannot slip between check and park.
     * Native DMA/uninstrumented writes still have the bounded timeout. */
    if (read_be64(memory, pc) == 0x800000012ull && read_be64(memory, chain) == published_pc) {
#ifndef _WIN32
        const unsigned us = park_us();
        if (us && us < timeout_ms * 1000u) sleep_srw_us(&w->wake, &lock, us);
        else
#endif
        SleepConditionVariableSRW(&w->wake, &lock, timeout_ms, 0);
    }
    __atomic_store_n(&w->active, 0, __ATOMIC_RELAXED);
    const uint32_t remaining = __atomic_load_n(&occupied_slots, __ATOMIC_RELAXED) & ~(1u << i);
    __atomic_store_n(&occupied_slots, remaining, __ATOMIC_RELEASE);
    uint64_t lines = 0;
    for (uint32_t active = remaining; active; active &= active - 1) {
        const Waiter* other = &waiters[__builtin_ctz(active)];
        lines |= spurs_command_line_bit(other->pc) | spurs_command_line_bit(other->chain);
    }
    __atomic_store_n(&g_spurs_command_wait_lines, lines, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&lock);
}

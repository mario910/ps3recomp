#include "spurs_job_pool.h"
#include "../platform/thread_name.h"
#include "spurs_job_ls.h"
#include "../platform/win32_compat.h"
#include "ps3emu/ps3_ftrace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void spurs_job_set_spu_num(uint32_t);
extern uint32_t spurs_job_get_spu_num(void);
typedef struct Batch {
    uint32_t first, stride, chain;
    unsigned count, next, remaining;
    int first_index;
    spurs_pool_job_fn run;
    struct Batch* next_batch;
    uint64_t queued_ns, finished_ns;
    const SpursPoolRange* ranges;
    unsigned range_count;
    /* Sequence state. finished, not remaining == 0, releases the owner:
     * between stages remaining is 0 while the last completer prepares the
     * next stage in range_buf, which lives in the owner's stack Batch. */
    int finished;
    spurs_pool_next_fn next_stage;
    void* context;
    SpursPoolRange range_buf[32];
} Batch;
static struct {
    SRWLOCK admission, lock;
    CONDITION_VARIABLE work, done;
    HANDLE threads[5];
    unsigned workers;
    uint32_t instance;
    int initialized, stopping;
    Batch* head;
    Batch* tail;
    unsigned active_batches;
    unsigned busy_lanes, borrowed_lanes;
    int caller_help;
} pool;
static _Thread_local int in_worker;

/* Caller holds pool.lock. A submitting thread may claim its own tail batch;
 * remove it without changing the order of other ready batches. */
static unsigned claim(Batch* batch)
{
    unsigned index = batch->next++;
    if (batch->next == batch->count) {
        Batch* previous = NULL;
        for (Batch* at = pool.head; at != batch; at = at->next_batch) previous = at;
        if (previous) previous->next_batch = batch->next_batch;
        else pool.head = batch->next_batch;
        if (pool.tail == batch) pool.tail = previous;
    }
    return index;
}

int spurs_job_pool_ranges_valid(const SpursPoolRange* ranges, unsigned range_count,
                                unsigned* total)
{
    if (!ranges || !range_count || range_count > 32) return 0;
    unsigned count = 0;
    for (unsigned n = 0; n < range_count; ++n) {
        const SpursPoolRange* r = &ranges[n];
        if (!r->count || !r->stride ||
            (uint64_t)r->first + (uint64_t)r->count * r->stride > 0x100000000ull ||
            r->first_index < 0 || r->count > 0x7fffffffu - (unsigned)r->first_index ||
            r->count > 0x7fffffffu - count) return 0;
        count += r->count;
    }
    *total = count;
    return 1;
}

/* Caller holds pool.lock; the batch is not queued. */
static void enqueue(Batch* batch)
{
    batch->next_batch = NULL;
    if (pool.tail) pool.tail->next_batch = batch;
    else pool.head = batch;
    pool.tail = batch;
}

/* Caller holds pool.lock and has just completed one job of batch. The owner
 * may return as soon as finished is set (and the lock is dropped), so nothing
 * here touches the batch after that store. */
static void complete(Batch* batch)
{
    if (--batch->remaining) return;
    if (batch->next_stage) {
        /* Every callback of this stage has returned: all workers that ran
         * one decremented remaining under this lock first. The batch is
         * fully claimed, so it is out of the queue; nothing else uses it. */
        ReleaseSRWLockExclusive(&pool.lock);
        unsigned ranges = batch->next_stage(batch->context, batch->range_buf), count = 0;
        if (ranges && !spurs_job_pool_ranges_valid(batch->range_buf, ranges, &count))
            abort(); /* the continuation broke its contract */
        AcquireSRWLockExclusive(&pool.lock);
        if (ranges) {
            const SpursPoolRange* r = batch->range_buf;
            batch->first = r->first; batch->stride = r->stride; batch->first_index = r->first_index;
            batch->ranges = r; batch->range_count = ranges;
            batch->count = batch->remaining = count; batch->next = 0;
            if (batch->queued_ns) batch->queued_ns = ps3_ft_now();
            enqueue(batch); /* at the tail: other chains keep their turn */
            WakeAllConditionVariable(&pool.work);
            return;
        }
    }
    if (batch->queued_ns) batch->finished_ns = ps3_ft_now();
    batch->finished = 1;
    --pool.active_batches;
    WakeAllConditionVariable(&pool.done);
}

static void execute(Batch* batch, unsigned index)
{
    for (unsigned n = 0; n < batch->range_count; ++n) {
        const SpursPoolRange* range = &batch->ranges[n];
        if (index < range->count) {
            batch->run(range->first + index * range->stride,
                       range->first_index + (int)index, range->stride, batch->chain);
            return;
        }
        index -= range->count;
    }
    abort(); /* claim() cannot produce an index outside validated ranges */
}

/* A chain separated by SYNCs arrives as many short stages (Buzz's studio:
 * ~165 per frame, ~14 jobs each), so lanes go idle at every stage boundary.
 * Sleeping on the condition variable there costs an OS wake-up per lane per
 * stage. SPURS_POOL_SPIN_US=n first polls for new work for up to n us. The
 * polling lanes occupy cores, so it applies only with 12+ logical processors,
 * where the guest PPU, RSX and audio threads keep cores of their own. */
static uint64_t spin_ticks(void)
{
    static uint64_t ticks = UINT64_MAX;
    if (ticks == UINT64_MAX) {
        const char* e = getenv("SPURS_POOL_SPIN_US");
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        const long us = e ? atol(e) : 0;
        const DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        ticks = us > 0 && us <= 100000 && cpus >= 12
            ? (uint64_t)f.QuadPart * (uint64_t)us / 1000000u : 0;
        if (us > 0)
            fprintf(stderr, "[job-pool] idle lanes poll %ld us before sleeping%s (%lu logical processors)\n",
                    ticks ? us : 0L, ticks ? "" : ": off, too few processors", (unsigned long)cpus);
    }
    return ticks;
}

/* Lock-free hint only: the caller re-checks under pool.lock. */
static void spin_for_work(unsigned lane_bit)
{
    const uint64_t ticks = spin_ticks();
    if (!ticks) return;
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    for (;;) {
        for (int n = 0; n < 32; ++n) {
            if ((__atomic_load_n(&pool.head, __ATOMIC_ACQUIRE) &&
                 !(__atomic_load_n(&pool.borrowed_lanes, __ATOMIC_RELAXED) & lane_bit)) ||
                __atomic_load_n(&pool.stopping, __ATOMIC_RELAXED))
                return;
            YieldProcessor();
        }
        QueryPerformanceCounter(&now);
        if ((uint64_t)(now.QuadPart - start.QuadPart) >= ticks) return;
    }
}

static DWORD WINAPI job_worker(LPVOID argument)
{
    const unsigned lane_bit = 1u << (unsigned)(uintptr_t)argument;
    in_worker = 1;
    spurs_job_set_spu_num((uint32_t)(uintptr_t)argument);
    for (;;) {
        spin_for_work(lane_bit);
        AcquireSRWLockExclusive(&pool.lock);
        while (!pool.stopping && (!pool.head || (pool.borrowed_lanes & lane_bit)))
            SleepConditionVariableSRW(&pool.work, &pool.lock, INFINITE, 0);
        if (pool.stopping) {
            ReleaseSRWLockExclusive(&pool.lock);
            spurs_job_release_thread_context();
            return 0;
        }
        Batch* batch = pool.head;
        unsigned index = claim(batch);
        pool.busy_lanes |= lane_bit;
        ReleaseSRWLockExclusive(&pool.lock);

        if (!index && batch->queued_ns)
            ps3_ft_span(PS3_FT_POOL_READY, GetCurrentThreadId(), batch->chain,
                        batch->first, batch->queued_ns, ps3_ft_now());
        execute(batch, index);

        AcquireSRWLockExclusive(&pool.lock);
        pool.busy_lanes &= ~lane_bit;
        complete(batch);
        ReleaseSRWLockExclusive(&pool.lock);
    }
}

int spurs_job_pool_run(uint32_t instance, unsigned nspus, unsigned workers,
                       uint32_t first, unsigned count, uint32_t stride,
                       uint32_t chain, int first_index, spurs_pool_job_fn run)
{
    SpursPoolRange range = {first, stride, count, first_index};
    return spurs_job_pool_run_ranges(instance, nspus, workers, &range, 1, chain, run);
}

int spurs_job_pool_run_ranges(uint32_t instance, unsigned nspus, unsigned workers,
                              const SpursPoolRange* ranges, unsigned range_count,
                              uint32_t chain, spurs_pool_job_fn run)
{
    return spurs_job_pool_run_sequence(instance, nspus, workers, ranges, range_count,
                                       chain, run, NULL, NULL);
}

int spurs_job_pool_run_sequence(uint32_t instance, unsigned nspus, unsigned workers,
                                const SpursPoolRange* ranges, unsigned range_count,
                                uint32_t chain, spurs_pool_job_fn run,
                                spurs_pool_next_fn next, void* context)
{
    unsigned count = 0;
    if (in_worker || !run || !instance || !chain || !workers || workers > 5 ||
        nspus <= workers || !spurs_job_pool_ranges_valid(ranges, range_count, &count)) return 0;
    AcquireSRWLockExclusive(&pool.admission);
    if (!pool.initialized) {
        pool.initialized = 1;
        pool.instance = instance;
        const char* help = getenv("SPURS_POOL_CALLER_HELP");
        pool.caller_help = help && help[0] && help[0] != '0';
        (void)spin_ticks();   /* read once, before any lane polls */
        for (unsigned n = 0; n < workers; ++n) {
            HANDLE thread = CreateThread(NULL, 1024 * 1024, job_worker,
                                          (LPVOID)(uintptr_t)(n + 1), 0, NULL);
            if (!thread) break;
            ps3_thread_name(thread, "SPURS pool lane %u", n + 1);
            pool.threads[pool.workers++] = thread;
        }
        fprintf(stderr, "[job-pool] instance=%08X workers=%u virtual-SPUs=1..%u (0 reserved)\n",
                instance, pool.workers, pool.workers);
    }
    if (pool.instance != instance || !pool.workers || pool.workers > workers ||
        pool.workers >= nspus) {
        ReleaseSRWLockExclusive(&pool.admission);
        return 0;
    }
    Batch batch = {ranges[0].first, ranges[0].stride, chain, count, 0, count,
                   ranges[0].first_index, run, NULL,
                   PS3_FT_ON() ? ps3_ft_now() : 0, 0, ranges, range_count, 0, NULL, NULL, {{0}}};
    /* A borrowing caller would have to run continuations itself; keep that
     * diagnostic mode on single stages. */
    if (next && !pool.caller_help) {
        memcpy(batch.range_buf, ranges, range_count * sizeof *ranges);
        batch.ranges = batch.range_buf;
        batch.next_stage = next;
        batch.context = context;
    }
    AcquireSRWLockExclusive(&pool.lock);
    enqueue(&batch);
    ++pool.active_batches;
    /* Borrow an ID only while its dedicated worker is idle. The ID, not the
     * OS thread, owns Edge rings/RSX labels. Never add a fifth geometry lane
     * or use the service lane 0. A caller only executes its own chain. */
    unsigned lane = 0, saved_lane = 0;
    if (pool.caller_help) {
        for (unsigned n = 1; n <= pool.workers; ++n)
            if (!((pool.busy_lanes | pool.borrowed_lanes) & (1u << n))) { lane = n; break; }
        if (lane) {
            pool.borrowed_lanes |= 1u << lane;
            saved_lane = spurs_job_get_spu_num();
            spurs_job_set_spu_num(lane);
            in_worker = 1;
        }
    }
    WakeAllConditionVariable(&pool.work);
    /* Admission protects initialization/enqueue only, not execution. Ready
     * lists from other chains can fill idle lanes while this list's last
     * job finishes. The caller's stack Batch lives until remaining is zero. */
    ReleaseSRWLockExclusive(&pool.admission);
    while (lane && batch.next < batch.count) {
        unsigned index = claim(&batch);
        ReleaseSRWLockExclusive(&pool.lock);
        if (!index && batch.queued_ns)
            ps3_ft_span(PS3_FT_POOL_READY, GetCurrentThreadId(), batch.chain,
                        batch.first, batch.queued_ns, ps3_ft_now());
        execute(&batch, index);
        AcquireSRWLockExclusive(&pool.lock);
        complete(&batch);
    }
    if (lane) {
        spurs_job_set_spu_num(saved_lane);
        in_worker = 0;
        pool.borrowed_lanes &= ~(1u << lane);
        WakeAllConditionVariable(&pool.work);
    }
    while (!batch.finished)
        SleepConditionVariableSRW(&pool.done, &pool.lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&pool.lock);
    if (batch.finished_ns)
        ps3_ft_span(PS3_FT_POOL_JOIN, GetCurrentThreadId(), batch.chain,
                    batch.first, batch.finished_ns, ps3_ft_now());
    return 1;
}

void spurs_job_pool_shutdown(void)
{
    AcquireSRWLockExclusive(&pool.admission);
    AcquireSRWLockExclusive(&pool.lock);
    while (pool.active_batches)
        SleepConditionVariableSRW(&pool.done, &pool.lock, INFINITE, 0);
    pool.stopping = 1;
    WakeAllConditionVariable(&pool.work);
    ReleaseSRWLockExclusive(&pool.lock);
    for (unsigned n = 0; n < pool.workers; ++n) {
        WaitForSingleObject(pool.threads[n], INFINITE);
        CloseHandle(pool.threads[n]);
        pool.threads[n] = NULL;
    }
    pool.workers = 0;
    pool.instance = 0;
    pool.initialized = 0;
    pool.stopping = 0;
    ReleaseSRWLockExclusive(&pool.admission);
}

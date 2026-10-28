#include "spurs_job_pool.h"
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

static void complete(Batch* batch)
{
    if (!--batch->remaining) {
        if (batch->queued_ns) batch->finished_ns = ps3_ft_now();
        --pool.active_batches;
        WakeAllConditionVariable(&pool.done);
    }
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

static DWORD WINAPI job_worker(LPVOID argument)
{
    const unsigned lane_bit = 1u << (unsigned)(uintptr_t)argument;
    in_worker = 1;
    spurs_job_set_spu_num((uint32_t)(uintptr_t)argument);
    for (;;) {
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
    if (in_worker || !run || !instance || !chain || !ranges || !range_count ||
        range_count > 32 || !workers || workers > 5 || nspus <= workers) return 0;
    unsigned count = 0;
    for (unsigned n = 0; n < range_count; ++n) {
        const SpursPoolRange* r = &ranges[n];
        if (!r->count || !r->stride ||
            (uint64_t)r->first + (uint64_t)r->count * r->stride > 0x100000000ull ||
            r->first_index < 0 || r->count > 0x7fffffffu - (unsigned)r->first_index ||
            r->count > 0x7fffffffu - count) return 0;
        count += r->count;
    }
    AcquireSRWLockExclusive(&pool.admission);
    if (!pool.initialized) {
        pool.initialized = 1;
        pool.instance = instance;
        const char* help = getenv("SPURS_POOL_CALLER_HELP");
        pool.caller_help = help && help[0] && help[0] != '0';
        for (unsigned n = 0; n < workers; ++n) {
            HANDLE thread = CreateThread(NULL, 1024 * 1024, job_worker,
                                          (LPVOID)(uintptr_t)(n + 1), 0, NULL);
            if (!thread) break;
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
                   PS3_FT_ON() ? ps3_ft_now() : 0, 0, ranges, range_count};
    AcquireSRWLockExclusive(&pool.lock);
    if (pool.tail) pool.tail->next_batch = &batch;
    else pool.head = &batch;
    pool.tail = &batch;
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
    while (batch.remaining)
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

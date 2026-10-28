#ifndef PS3_SPURS_JOB_POOL_H
#define PS3_SPURS_JOB_POOL_H
#include <stdint.h>
typedef void (*spurs_pool_job_fn)(uint32_t job, int index, uint32_t stride, uint32_t chain);
typedef struct {
    uint32_t first, stride;
    unsigned count;
    int first_index;
} SpursPoolRange;
/* Multiple already-published independent lists, with NO intervening command.
 * All range storage remains caller-owned until the full-batch join returns. */
int spurs_job_pool_run_ranges(uint32_t instance, unsigned nspus, unsigned workers,
                              const SpursPoolRange* ranges, unsigned range_count,
                              uint32_t chain, spurs_pool_job_fn run);
/* Opt-in independent JOBLIST execution. Reserves virtual SPU 0 for services;
 * caller must classify all jobs as safe for this pool. Returns 0 without
 * running anything if unavailable, 1 only after EVERY callback completed. */
int spurs_job_pool_run(uint32_t instance, unsigned nspus, unsigned workers,
                       uint32_t first, unsigned count, uint32_t stride,
                       uint32_t chain, int first_index, spurs_pool_job_fn run);
/* Same bounds the pool enforces; *total receives the job count. */
int spurs_job_pool_ranges_valid(const SpursPoolRange* ranges, unsigned range_count,
                                unsigned* total);
/* Stage continuation. Called by the LAST completer of a stage, after every
 * callback of that stage has returned, outside the pool lock, on a pool
 * worker. It may read memory but must not run jobs or block. It writes the
 * next stage into out (ranges that satisfy spurs_job_pool_ranges_valid) and
 * returns its range count, or 0 to end the sequence. */
typedef unsigned (*spurs_pool_next_fn)(void* context, SpursPoolRange out[32]);
/* Like spurs_job_pool_run_ranges, then keeps running the stages `next`
 * prepares without returning to the caller in between. Every stage is a full
 * barrier: no job of stage k+1 starts before all of stage k returned.
 * Returns after the sequence ended (next returned 0); context stays
 * caller-owned until then. With SPURS_POOL_CALLER_HELP, next is ignored. */
int spurs_job_pool_run_sequence(uint32_t instance, unsigned nspus, unsigned workers,
                                const SpursPoolRange* ranges, unsigned range_count,
                                uint32_t chain, spurs_pool_job_fn run,
                                spurs_pool_next_fn next, void* context);
/* Join idle workers. Call only after submission owners have stopped. */
void spurs_job_pool_shutdown(void);
#endif

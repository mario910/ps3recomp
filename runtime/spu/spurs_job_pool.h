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
/* Join idle workers. Call only after submission owners have stopped. */
void spurs_job_pool_shutdown(void);
#endif

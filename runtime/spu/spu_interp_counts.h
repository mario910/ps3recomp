/* Optional cumulative instruction-executor counts. Never a timing profiler. */
#ifndef SPU_INTERP_COUNTS_H
#define SPU_INTERP_COUNTS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SPU_INTERP_COUNT_CAPACITY 4096u
#define SPU_INTERP_COUNT_DESCRIPTOR UINT64_C(0x5349455800011000)
enum { SPU_INTERP_COUNT_FULL=1, SPU_INTERP_COUNT_OVERFLOW=2,
       SPU_INTERP_COUNT_TOTAL_OVERFLOW=4 };
typedef struct spu_interp_count_row {
    int32_t image;
    uint32_t page;
    uint64_t steps;
} spu_interp_count_row;
typedef struct spu_interp_count_info {
    int32_t enabled; /* -1: not armed, 0: disabled, 1: enabled */
    uint32_t flags;
    uint64_t unbucketed_steps;
    uint64_t total;
} spu_interp_count_info;
/* Parse SPU_INTERP_EXACT=0/1 once. Call at process startup to establish explicit
 * zero coverage even when no interpreter invocation ever occurs. */
int spu_interp_counts_enabled(void);
uint64_t spu_interp_counts_descriptor(void);
/* Atomic cells, no reset. For a coherent snapshot, callers must quiesce ALL
 * interpreter writers. Returns required row count; writes at most capacity
 * rows. A nonzero flags value invalidates loss-free counting. */
size_t spu_interp_counts_read(spu_interp_count_row* rows,size_t capacity,
                             spu_interp_count_info* info);
#ifdef __cplusplus
}
#endif
#endif

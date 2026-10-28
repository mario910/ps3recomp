#ifndef PS3_SPURS_JOB_LS_H
#define PS3_SPURS_JOB_LS_H
#include <stdint.h>
#include <stddef.h>
/* Chain-owned completed-job LS snapshots. Readers copy under the slot lock;
 * never return a pointer that another completing job can overwrite. */
void spurs_job_ls_publish(uint32_t chain, const uint8_t* ls);
int spurs_job_ls_read(uint32_t chain, uint32_t offset, size_t size, uint8_t* out);
/* On success takes exclusive ownership of storage/ls and returns the previous
 * opaque storage (or NULL). No reader retains it after this call returns.
 * Failure takes nothing. The new owner must not touch storage until returned
 * by a later exchange. All DMA/coherency users must already be detached. */
int spurs_job_ls_exchange(uint32_t chain, void* storage, uint8_t* ls, void** previous);
/* Free only this thread's spare context, never a published snapshot. */
void spurs_job_release_thread_context(void);
#endif

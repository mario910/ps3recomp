#ifndef SPURS_CHAIN_WORKER_H
#define SPURS_CHAIN_WORKER_H
#include "../platform/win32_compat.h"
#ifdef __cplusplus
extern "C" {
#endif
/* One persistent serial executor per registry slot. Accepted submissions are
 * never discarded; the caller still owns guest Run/Kick coalescing. */
int spurs_chain_worker_submit(unsigned slot, DWORD (WINAPI *run)(LPVOID));
/* Drain and join. Not callable from a callback or concurrently with submit. */
void spurs_chain_workers_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif

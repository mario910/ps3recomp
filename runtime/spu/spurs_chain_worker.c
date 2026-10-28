#include "spurs_chain_worker.h"
#include <stdint.h>
#include <limits.h>

enum { CHAIN_WORKERS = 32 };
typedef struct {
    SRWLOCK lock;
    CONDITION_VARIABLE wake;
    HANDLE thread;
    DWORD (WINAPI *run)(LPVOID);
    unsigned pending;
    int stopping;
} ChainWorker;
static ChainWorker workers[CHAIN_WORKERS];

static DWORD WINAPI execute(LPVOID argument)
{
    ChainWorker* w = &workers[(unsigned)(uintptr_t)argument];
    AcquireSRWLockExclusive(&w->lock);
    for (;;) {
        while (!w->pending && !w->stopping)
            SleepConditionVariableSRW(&w->wake, &w->lock, INFINITE, 0);
        if (!w->pending && w->stopping) break;
        --w->pending;
        DWORD (WINAPI *run)(LPVOID) = w->run;
        ReleaseSRWLockExclusive(&w->lock);
        run(argument);
        AcquireSRWLockExclusive(&w->lock);
    }
    ReleaseSRWLockExclusive(&w->lock);
    return 0;
}

int spurs_chain_worker_submit(unsigned slot, DWORD (WINAPI *run)(LPVOID))
{
    if (slot >= CHAIN_WORKERS || !run) return 0;
    ChainWorker* w = &workers[slot];
    AcquireSRWLockExclusive(&w->lock);
    if (w->stopping || w->pending == UINT_MAX || (w->thread && w->run != run)) {
        ReleaseSRWLockExclusive(&w->lock);
        return 0;
    }
    if (!w->thread) {
        w->run = run;
        w->thread = CreateThread(NULL, 1u << 20, execute,
                                 (LPVOID)(uintptr_t)slot, 0, NULL);
        if (!w->thread) {
            ReleaseSRWLockExclusive(&w->lock);
            return 0;
        }
    }
    ++w->pending;
    WakeConditionVariable(&w->wake);
    ReleaseSRWLockExclusive(&w->lock);
    return 1;
}

void spurs_chain_workers_shutdown(void)
{
    /* Mark all first so independent workers drain concurrently. */
    for (unsigned i = 0; i < CHAIN_WORKERS; ++i) {
        ChainWorker* w = &workers[i];
        AcquireSRWLockExclusive(&w->lock);
        w->stopping = 1;
        WakeConditionVariable(&w->wake);
        ReleaseSRWLockExclusive(&w->lock);
    }
    for (unsigned i = 0; i < CHAIN_WORKERS; ++i) {
        ChainWorker* w = &workers[i];
        if (w->thread) { WaitForSingleObject(w->thread, INFINITE); CloseHandle(w->thread); }
        AcquireSRWLockExclusive(&w->lock);
        w->thread = NULL;
        w->run = NULL;
        w->stopping = 0;
        ReleaseSRWLockExclusive(&w->lock);
    }
}

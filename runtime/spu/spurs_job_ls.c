#include "spurs_job_ls.h"
#include "../platform/win32_compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { LS_BYTES = 256 * 1024, SNAPSHOT_SLOTS = 64 };
typedef struct { uint32_t chain; uint8_t* bytes; SRWLOCK lock; int valid; void* storage; } Snapshot;
static Snapshot slots[SNAPSHOT_SLOTS];
static SRWLOCK directory = SRWLOCK_INIT;

static Snapshot* snapshot_find(uint32_t chain, int create)
{
    if (!chain) return NULL;
    Snapshot* free_slot = NULL;
    AcquireSRWLockExclusive(&directory);
    for (unsigned n = 0; n < SNAPSHOT_SLOTS; ++n) {
        if (slots[n].chain == chain) {
            ReleaseSRWLockExclusive(&directory);
            return &slots[n];
        }
        if (!slots[n].chain && !free_slot) free_slot = &slots[n];
    }
    if (create && free_slot) {
        free_slot->bytes = (uint8_t*)malloc(LS_BYTES);
        if (free_slot->bytes) {
            /* Readers reject the slot until its first full copy is valid. */
            free_slot->chain = chain;
        } else free_slot = NULL;
    } else free_slot = NULL;
    ReleaseSRWLockExclusive(&directory);
    return free_slot;
}

void spurs_job_ls_publish(uint32_t chain, const uint8_t* ls)
{
    if (!chain || !ls) return;
    Snapshot* slot = snapshot_find(chain, 1);
    if (!slot) {
        static volatile LONG reported;
        if (!InterlockedExchange(&reported, 1))
            fprintf(stderr, "[spurs-job-ls] cannot retain snapshot (64-chain bound or allocation failure)\n");
        return;
    }
    AcquireSRWLockExclusive(&slot->lock);
    memcpy(slot->bytes, ls, LS_BYTES);
    slot->valid = 1;
    ReleaseSRWLockExclusive(&slot->lock);
}

int spurs_job_ls_read(uint32_t chain, uint32_t offset, size_t size, uint8_t* out)
{
    if (!out || offset > LS_BYTES || size > LS_BYTES - offset) return 0;
    Snapshot* slot = snapshot_find(chain, 0);
    if (!slot) return 0;
    AcquireSRWLockShared(&slot->lock);
    if (!slot->valid) {
        ReleaseSRWLockShared(&slot->lock);
        return 0;
    }
    memcpy(out, slot->bytes + offset, size);
    ReleaseSRWLockShared(&slot->lock);
    return 1;
}

int spurs_job_ls_exchange(uint32_t chain, void* storage, uint8_t* ls, void** previous)
{
    if (!chain || !storage || !ls || !previous) return 0;
    Snapshot* slot = snapshot_find(chain, 1);
    if (!slot) return 0;
    AcquireSRWLockExclusive(&slot->lock);
    *previous = slot->storage;
    if (!slot->storage) free(slot->bytes); /* legacy copy-owned buffer */
    slot->bytes = ls;
    slot->storage = storage;
    slot->valid = 1;
    ReleaseSRWLockExclusive(&slot->lock);
    return 1;
}

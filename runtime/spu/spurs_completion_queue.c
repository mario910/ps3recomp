#include "spurs_completion_queue.h"
#include "../platform/win32_compat.h"

enum { QUEUES = 32 };
typedef struct {
    uint64_t ticket, pending;
    uint32_t first, stride;
    unsigned count;
    CONDITION_VARIABLE wake;
} CompletionQueue;
static CompletionQueue queues[QUEUES];
static SRWLOCK lock = SRWLOCK_INIT;
static uint64_t next_ticket, watched_lines;
static uint64_t line_bit(uint32_t ea) { return 1ull << (((ea >> 7) ^ (ea >> 17)) & 63); }
static uint64_t mask(unsigned count) { return count == 64 ? UINT64_MAX : (1ull << count) - 1; }
static void update_filter(void)
{
    uint64_t bits = 0;
    for (unsigned q=0;q<QUEUES;++q) if (queues[q].ticket)
        for(unsigned n=0;n<queues[q].count;++n) bits |= line_bit(queues[q].first+n*queues[q].stride);
    __atomic_store_n(&watched_lines,bits,__ATOMIC_RELEASE);
}
uint64_t spurs_completion_begin(uint32_t first, uint32_t stride, unsigned count)
{
    if (!first || (first & 127) || !stride || (stride & 127) || !count || count>64 ||
        (uint64_t)first+(uint64_t)(count-1)*stride > UINT32_MAX-127u) return 0;
    AcquireSRWLockExclusive(&lock);
    uint64_t result=0;
    for(unsigned q=0;q<QUEUES;++q) if (!queues[q].ticket) {
        CompletionQueue* s=&queues[q];
        if (!++next_ticket) ++next_ticket;
        s->ticket=next_ticket;s->first=first;s->stride=stride;s->count=count;
        s->pending=mask(count); result=s->ticket;update_filter();break;
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}
uint64_t spurs_completion_take(uint64_t ticket, unsigned timeout_ms)
{
    if (!ticket) return 0;
    AcquireSRWLockExclusive(&lock);
    uint64_t result=0;
    for(unsigned q=0;q<QUEUES;++q) if (queues[q].ticket==ticket) {
        CompletionQueue* s=&queues[q];
        // Check/park and enqueue use the same lock: publication between the
        // guest's negative probe and this call cannot be lost.
        if (!s->pending && timeout_ms) SleepConditionVariableSRW(&s->wake,&lock,timeout_ms,0);
        if (s->ticket==ticket) { result=s->pending;s->pending=0; }
        break;
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}
void spurs_completion_end(uint64_t ticket)
{
    AcquireSRWLockExclusive(&lock);
    for(unsigned q=0;q<QUEUES;++q) if (queues[q].ticket==ticket) {
        queues[q].ticket=0;queues[q].pending=0;
        WakeAllConditionVariable(&queues[q].wake);update_filter();break;
    }
    ReleaseSRWLockExclusive(&lock);
}
void spurs_completion_notify(uint32_t line)
{
    line &= ~127u;
    if (!(__atomic_load_n(&watched_lines,__ATOMIC_ACQUIRE)&line_bit(line))) return;
    AcquireSRWLockExclusive(&lock);
    for(unsigned q=0;q<QUEUES;++q) {
        CompletionQueue* s=&queues[q];
        if (!s->ticket || line<s->first) continue;
        uint32_t delta=line-s->first, n=delta/s->stride;
        if (delta%s->stride || n>=s->count) continue;
        s->pending |= 1ull<<n;
        WakeConditionVariable(&s->wake);
    }
    ReleaseSRWLockExclusive(&lock);
}

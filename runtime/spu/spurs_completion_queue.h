#ifndef SPURS_COMPLETION_QUEUE_H
#define SPURS_COMPLETION_QUEUE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Bounded deduplicating candidate queues. Bits are hints, NOT completions.
 * Registration includes an initial full check for pre-existing events.
 * take returns zero on timeout; callers must then rescan authoritative state.
 * No queue lock may be held while executing guest code or acquiring EF locks. */
uint64_t spurs_completion_begin(uint32_t first_flag, uint32_t stride, unsigned count);
uint64_t spurs_completion_take(uint64_t ticket, unsigned timeout_ms);
void spurs_completion_end(uint64_t ticket);
void spurs_completion_notify(uint32_t line);
#ifdef __cplusplus
}
#endif
#endif

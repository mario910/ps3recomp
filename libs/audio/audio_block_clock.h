/* Host-clock fallback for 256-sample audio blocks at 48 kHz. */
#ifndef PS3RECOMP_AUDIO_BLOCK_CLOCK_H
#define PS3RECOMP_AUDIO_BLOCK_CLOCK_H
#include <stdint.h>

typedef struct audio_block_clock {
    uint64_t next_ns;
    unsigned remainder;
    int started;
} audio_block_clock;

/* Reserve one deadline. Preserve fractional nanoseconds, but never catch up
 * missed blocks after a debugger stop, host pause, or a slow mixer. */
static inline uint64_t audio_block_clock_take(audio_block_clock* clock, uint64_t now)
{
    if (!clock->started || (now > clock->next_ns &&
                           now - clock->next_ns >= UINT64_C(5333334))) {
        clock->next_ns = now;
        clock->remainder = 0;
        clock->started = 1;
    }
    const uint64_t due = clock->next_ns;
    clock->next_ns += UINT64_C(5333333);
    clock->remainder += 16000;
    if (clock->remainder >= 48000) {
        clock->next_ns++;
        clock->remainder -= 48000;
    }
    return due;
}
#endif

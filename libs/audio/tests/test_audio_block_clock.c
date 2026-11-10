#include "../audio_block_clock.h"
#include <stdio.h>

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #expression); return 1; \
} } while (0)

int main(void)
{
    audio_block_clock clock = {0};
    for (uint64_t block = 0; block < 3000; ++block) {
        const uint64_t due = block * UINT64_C(256000000000) / 48000;
        CHECK(audio_block_clock_take(&clock, due) == due);
    }
    CHECK(clock.next_ns == UINT64_C(16000000000));
    /* Small scheduling jitter preserves phase rather than accumulating drift. */
    CHECK(audio_block_clock_take(&clock, UINT64_C(16000100000)) == UINT64_C(16000000000));
    /* A long interruption drops backlog. The next call must wait a block. */
    CHECK(audio_block_clock_take(&clock, UINT64_C(20000000000)) == UINT64_C(20000000000));
    CHECK(audio_block_clock_take(&clock, UINT64_C(20000000000)) == UINT64_C(20005333333));
    /* Reset on host pause. */
    clock = (audio_block_clock){0};
    CHECK(audio_block_clock_take(&clock, UINT64_C(30000000000)) == UINT64_C(30000000000));
    puts("audio block clock: passed");
    return 0;
}

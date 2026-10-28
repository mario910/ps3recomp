/* spu_shufb (pshufb / NEON tbl path) must match the byte-loop reference for
 * every selector class: a/b sources, 0x00 / 0xFF / 0x80 specials. Build with
 * -msse4.1 on x86 so the SIMD version is the one under test (NEON is always
 * on for arm64). */
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
#if !defined(__SSE4_1__) && !defined(__ARM_NEON)
    puts("test_spu_shufb: built without SSE4.1 or NEON, nothing to compare");
    return 0;
#else
    srand(12345);
    /* Exhaustive selectors at every destination byte, with distinguishable
     * source bytes. Catches host/SPU ordering mistakes independently of RNG. */
    for (unsigned selector=0;selector<256;++selector) {
        u128 a,b,c;
        for(unsigned i=0;i<16;++i){a._u8[i]=(uint8_t)(i+1);b._u8[i]=(uint8_t)(i+65);c._u8[i]=(uint8_t)selector;}
        u128 actual=spu_shufb(a,b,c),expected=spu_shufb_ref(a,b,c);
        if(memcmp(&actual,&expected,16)){printf("selector %u mismatch\n",selector);return 1;}
    }
    for (int it = 0; it < 200000; it++) {
        u128 a, b, c;
        for (int i = 0; i < 16; i++) {
            a._u8[i] = (uint8_t)rand(); b._u8[i] = (uint8_t)rand();
            c._u8[i] = (uint8_t)rand();          /* covers all 256 selector values */
        }
        u128 x = spu_shufb(a, b, c), y = spu_shufb_ref(a, b, c);
        if (memcmp(&x, &y, 16)) {
            printf("test_spu_shufb: MISMATCH at iteration %d\n", it);
            return 1;
        }
    }
    puts("test_spu_shufb: ok (200000 random cases)");
    return 0;
#endif
}

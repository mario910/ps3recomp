#ifndef PS3_RSX_MORTON2D_H
#define PS3_RSX_MORTON2D_H
#include <stdint.h>
/* Rectangular Morton addressing, 0..16 bits per axis. Remaining bits of
 * the longer dimension follow the interleaved square's bits. */
static inline uint32_t rsx_spread16(uint32_t x)
{
    x &= 0xffffu;
    x = (x | (x << 8)) & 0x00ff00ffu;
    x = (x | (x << 4)) & 0x0f0f0f0fu;
    x = (x | (x << 2)) & 0x33333333u;
    return (x | (x << 1)) & 0x55555555u;
}
static inline uint32_t rsx_morton2d(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh)
{
    uint32_t n = lw < lh ? lw : lh, mask = (1u << n) - 1u;
    uint32_t out = rsx_spread16(x & mask) | (rsx_spread16(y & mask) << 1);
    if (lw > lh) out |= (x >> n) << (2 * n);
    else if (lh > lw) out |= (y >> n) << (2 * n);
    return out;
}
#endif

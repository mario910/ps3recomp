#ifndef RSX_RT_REGION_H
#define RSX_RT_REGION_H
#include <stdint.h>

typedef struct { uint32_t x, y, w, h; } rsx_rt_rect;

/* An interior linear texture aliases a surface only with the SAME byte-row
 * stride. Offsets in row padding, unaligned pixels and wrapping ranges do not
 * establish ownership. Callers separately check format/memory-space/AA. */
static inline int rsx_rt_region(uint32_t base, uint32_t width, uint32_t height,
    uint32_t pitch, uint32_t bytes_per_pixel, uint32_t address,
    uint32_t sample_w, uint32_t sample_h, uint32_t sample_pitch, rsx_rt_rect* out)
{
    if (!out || !width || !height || !sample_w || !sample_h || !bytes_per_pixel ||
        (uint64_t)width * bytes_per_pixel > pitch || pitch != sample_pitch ||
        address < base || (uint64_t)base + (uint64_t)pitch * height > UINT64_C(0x100000000)) return 0;
    uint32_t delta = address - base, row = delta / pitch, byte = delta % pitch;
    if (byte % bytes_per_pixel) return 0;
    uint32_t col = byte / bytes_per_pixel;
    if ((uint64_t)col + sample_w > width || (uint64_t)row + sample_h > height) return 0;
    out->x=col; out->y=row; out->w=sample_w; out->h=sample_h;
    return 1;
}

/* CLEAR_SURFACE honours scissor, not the draw viewport. Empty scissors are
 * no-ops; oversized defaults clip to the target instead of clearing neighbours. */
static inline rsx_rt_rect rsx_rt_clear_rect(uint32_t width, uint32_t height,
    uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    rsx_rt_rect r={0,0,0,0};
    if (x>=width || y>=height || !w || !h) return r;
    r.x=x; r.y=y; r.w=w<width-x?w:width-x; r.h=h<height-y?h:height-y;
    return r;
}
#endif

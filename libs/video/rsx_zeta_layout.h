#ifndef RSX_ZETA_LAYOUT_H
#define RSX_ZETA_LAYOUT_H
#include <stdint.h>
/* NV4097 surface AA codes: 0=1x, 3=diagonal 2x, 4/5=square 4x.
 * This describes guest storage, not the number of native GPU samples. */
static inline unsigned rsx_zeta_scale_x(uint32_t f){return ((f>>12)&15u)>=3?2:1;}
static inline unsigned rsx_zeta_scale_y(uint32_t f){return ((f>>12)&15u)>=4?2:1;}
static inline int rsx_zeta_supported(uint32_t f){
    unsigned aa=(f>>12)&15u;
    return ((f>>5)&7u)==2 && (aa==0||aa==3||aa==4||aa==5);
}
/* A Z24S8 word's big-endian bytes are Zhi,Zmid,Zlo,S. The existing ARGB8
 * uploader presents them as R=Zmid,G=Zlo,B=S,A=Zhi; use that same view so the
 * guest's CONTROL1 crossbar (including AA93) remains authoritative. */
static inline uint32_t rsx_zeta_rgba8(uint32_t z,uint32_t s){
    return ((z>>8)&255u)|((z&255u)<<8)|((s&255u)<<16)|((z>>16)<<24);
}
#endif

#ifndef PS3RECOMP_RSX_DEPTH_STATE_H
#define PS3RECOMP_RSX_DEPTH_STATE_H
#include "rsx_commands.h"
/* Stable per-draw PSO identity. RSX compare enums 0x200..0x207 correspond
 * to D3D12 NEVER..ALWAYS (1..8). Preserve the guest write mask separately. */
static inline u32 rsx_depth_key(const rsx_state* st)
{
    if(!st)return 3u | (3u<<2); /* legacy null-state: enabled, LEQUAL, writes */
    return (st->depth_test_enable?1u:0u) | (st->depth_mask?2u:0u) |
           ((st->depth_func&7u)<<2);
}
static inline unsigned rsx_depth_comparison(u32 key){return ((key>>2)&7u)+1u;}
#endif

#ifndef RSX_FRAME_BOUNDARY_H
#define RSX_FRAME_BOUNDARY_H
#include <stdint.h>
/* A committed FIFO flip is a boundary independently of scene complexity.
 * Legacy catch-up remains available for backends that cannot consume each one. */
static inline int rsx_stop_at_flip(int strict,uint32_t backlog,uint32_t catchup_threshold){
    return strict || backlog<catchup_threshold;
}
static inline int rsx_clear_is_ordered(int flip_boundary,uint32_t color_target,uint32_t mask){
    return flip_boundary || color_target!=0 || !(mask&0xf0u);
}
static inline int rsx_op_is_display_content(int flip_boundary,int is_clear,uint32_t target,uint32_t clear_mask){
    return target==0 && (!is_clear || (flip_boundary && (clear_mask&0xf0u)));
}
#endif

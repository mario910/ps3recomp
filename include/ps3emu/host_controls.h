#ifndef PS3RECOMP_HOST_CONTROLS_H
#define PS3RECOMP_HOST_CONTROLS_H
/* Host-only runtime preferences. Never guest APIs or game ABI replacements.
 * Defaults preserve existing SDK behaviour until the embedding port opts in. */
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PS3_HOST_LEGACY_FRAME_LIMIT UINT32_MAX
int ps3_host_set_volume(unsigned percent); /* 0..100; never changes device pacing */
unsigned ps3_host_get_volume(void);
int ps3_host_set_frame_limit(unsigned fps); /* 0 or 30; legacy initial value above */
unsigned ps3_host_get_frame_limit(void);
void ps3_host_set_fullscreen(int enabled);
int ps3_host_get_fullscreen(void); /* -1 initially: keep backend's existing mode */
/* Single mixer consumer; ramp changes over one block to avoid a hard edge.
 * gain belongs to that consumer, not to the settings-writer thread. */
void ps3_host_apply_volume(float* stereo,size_t frames,float* gain);

typedef struct ps3_host_pacer {uint64_t next_ns;unsigned fps;} ps3_host_pacer;
/* Pure deadline arithmetic. Zero means no wait; never accumulate catch-up debt. */
uint64_t ps3_host_frame_deadline(ps3_host_pacer*,uint64_t now_ns,unsigned fps);
#ifdef __cplusplus
}
#endif
#endif

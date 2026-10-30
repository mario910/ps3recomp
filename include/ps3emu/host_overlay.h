#ifndef PS3RECOMP_HOST_OVERLAY_H
#define PS3RECOMP_HOST_OVERLAY_H
/* Optional embedding-port UI hooks. Registered before backend/worker startup.
 * The SDK owns window/GPU lifetime; draw runs last with the target in RT state.
 * Hooks must not retain the command list across calls or modify guest state. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ps3_host_overlay_frame {
    void* window;
    void* device;
    void* queue;
    void* commands;
    uintptr_t rtv;
    unsigned width,height,frames_in_flight,format;
} ps3_host_overlay_frame;
typedef struct ps3_host_overlay_hooks {
    int (*message)(void* window,unsigned message,uintptr_t wparam,intptr_t lparam);
    void (*draw)(const ps3_host_overlay_frame*);
    void (*shutdown)(void);
} ps3_host_overlay_hooks;
void ps3_host_set_overlay(const ps3_host_overlay_hooks*);
int ps3_host_overlay_message(void*,unsigned,uintptr_t,intptr_t);
void ps3_host_overlay_draw(const ps3_host_overlay_frame*);
int ps3_host_overlay_enabled(void);
void ps3_host_overlay_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif

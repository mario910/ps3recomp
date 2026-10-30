#ifndef PS3RECOMP_HOST_OVERLAY_H
#define PS3RECOMP_HOST_OVERLAY_H
/* Optional embedding-port UI hooks. Registered before backend/worker startup.
 * The SDK owns window/GPU lifetime; draw runs last with the target in RT state.
 * Hooks must not retain the command list across calls or modify guest state. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { PS3_HOST_OVERLAY_D3D12 = 0, PS3_HOST_OVERLAY_VULKAN = 1 };
/* Vulkan: draw runs inside render_pass (load, then present) on the swapchain
 * image, recorded into frame.commands. The queue lock is held for the call,
 * so the hook may submit uploads on queue; it must not present. */
typedef struct ps3_host_overlay_vulkan {
    void* instance;
    void* physical_device;
    void* (*get_instance_proc_addr)(void* instance,const char* name);
    uint32_t queue_family;
    void* render_pass;
    unsigned min_image_count,image_count;
} ps3_host_overlay_vulkan;
typedef struct ps3_host_overlay_frame {
    void* window;
    void* device;
    void* queue;
    void* commands;
    uintptr_t rtv;
    unsigned width,height,frames_in_flight,format;
    unsigned api; /* PS3_HOST_OVERLAY_*; zero-initialized D3D12 frames stay D3D12 */
    const ps3_host_overlay_vulkan* vulkan;
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

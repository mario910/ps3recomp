/*
 * ps3recomp - Vulkan RSX Backend
 *
 * Linux and experimental Windows renderer. Same host-facing
 * entry points and test hooks as the Metal and headless null backends, so
 * runtime/host/host_posix.c drives it unchanged.
 *
 * Two draw paths, one registered at a time (the FIFO walker feeds both):
 *
 *   - the shared register-file draw engine (rsx_draw_engine.h), the path the
 *     Metal backend takes. The engine owns surfaces, render-to-texture, MRT,
 *     depth textures, vertex compaction and pipeline keys; this backend builds
 *     Vulkan pipelines from the guest's programs (HLSL -> SPIR-V through
 *     glslang, rsx_shader_spirv.h) and executes. Every ps3recomp_host scene
 *     passes on it. Default when guest programs are on.
 *   - the rsx_state vtable: the headless null backend's fixed-function
 *     contract (flat colour, depth test, texture unit 0, point sampling), so
 *     the two agree pixel for pixel. Default otherwise; with guest programs it
 *     also runs them, but has no surfaces (--rtt, --depthtex, --mrt, --mrt-a).
 *
 * Rendering is offscreen; a present reads the frame back (the host's pixel
 * checks), optionally dumps it, and optionally shows it in a window.
 *
 * Vulkan is loaded at run time (libvulkan.so.1 on Linux, the System32
 * vulkan-1.dll on Windows), not linked, so:
 *   - building needs only the Vulkan headers, not a target-arch libvulkan
 *     (which is what makes cross-compiling for aarch64 straightforward);
 *   - a machine with no Vulkan loader fails init() cleanly instead of failing
 *     to start at all.
 * Only Vulkan 1.0 core entry points are used, so it runs on 1.2-level drivers
 * such as NVIDIA's L4T driver for Tegra X1.
 *
 * Environment:
 *   PS3RECOMP_VK_DEVICE=<n>   pick physical device n (vkEnumeratePhysicalDevices
 *                             order) instead of the first non-CPU device.
 *   PS3RECOMP_VK_DUMP=<path>  write every presented frame to <path> as a
 *                             binary PPM (overwritten each present).
 *   PS3RECOMP_VK_DUMP_EVERY=N dump every Nth frame (default 1).
 *   PS3RECOMP_VK_WINDOW=1     also show each presented frame in a native Win32
 *                             or SDL2 (Linux)
 *                             window. Rendering and readback are unchanged, so
 *                             every test gives the same result with or without
 *                             it; if the window cannot be opened the run
 *                             continues headless and says why.
 *   PS3RECOMP_VK_FULLSCREEN=1 Linux SDL2 window: borderless full screen.
 *                             Windows host presentation controls are pending.
 *   PS3RECOMP_VK_HOLD=<sec>   with a window: keep the last frame on screen
 *                             this long before shutdown closes it.
 *   PS3RECOMP_VK_GUEST_PROGRAMS=1
 *                             run the guest's own vertex/fragment programs
 *                             (needs the translator: glslang at build time),
 *                             which also makes the draw engine the default.
 *   PS3RECOMP_RSX_ENGINE=dispatch|vtable
 *                             pick the path explicitly (the engine's own
 *                             switch, shared with the Metal backend).
 */

#ifndef PS3RECOMP_RSX_VULKAN_BACKEND_H
#define PS3RECOMP_RSX_VULKAN_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load Vulkan, create the device and the offscreen target, and register as
 * the RSX backend. `title` names the optional native window.
 * Returns 0 on success, -1 on any failure (reason printed to stderr). */
int  rsx_vulkan_backend_init(u32 width, u32 height, const char* title);

/* Unregister and destroy every Vulkan object, then unload the library. */
void rsx_vulkan_backend_shutdown(void);

/* Headless: always 0. Windowed: drains the native event queue; -1 once the
 * window has been closed or Esc pressed. */
int  rsx_vulkan_backend_pump_messages(void);
/* True only with an actual surface and swapchain (not a headless fallback). */
int  rsx_vulkan_backend_windowed(void);
#ifdef _WIN32
void rsx_vulkan_set_key_handler(void (*handler)(u32,int));
#endif

/* Read the frame back to host memory (and to PS3RECOMP_VK_DUMP if set), and
 * show it in the window if there is one. Same as _present_begin() followed
 * by _present_finish(). */
void rsx_vulkan_backend_present(void);
/* Successful window presentations only; zero for a headless run. */
u64 rsx_vulkan_backend_presented_frames(void);

/* The same present in two halves, for a host that presents on a thread of its
 * own beside the FIFO walker.
 *
 * _present_begin() reads the flipped surface: it must not run while the
 * walker is draining (call it under the same lock as
 * cellGcm_rsx_process_fifo). On the draw engine path it only records and
 * submits the copy out of that surface, and returns at once.
 *
 * _present_finish() waits for that copy, reads the frame back, writes the
 * dump and presents to the window. It may run while the walker drains the
 * next frame: the next frame's commands are ordered after the copy, so they
 * never reach the presented image. On the fixed-function path, where the
 * walker draws into the presented image itself, _present_begin() does all of
 * it and _present_finish() has nothing left to do. */
void rsx_vulkan_backend_present_begin(void);
void rsx_vulkan_backend_present_finish(void);

/* Centre pixel of the last frame a present copied out, as 0xFFRRGGBB, or 0
 * before the first or when PS3RECOMP_VK_PRESENT_READBACK=0 explicitly disables
 * presentation diagnostics (a frame dump forces them on). Unlike
 * _readback_center(), which on the draw engine path
 * reads the surface as it is now, this is what was shown. */
u32  rsx_vulkan_backend_presented_center(void);

/* The clear colour most recently set through the command stream, ARGB8888. */
u32  rsx_vulkan_backend_debug_color(void);

/* Draw-engine path: explicitly read the current selected surface, even if
 * presentation diagnostics are disabled. Vtable path: last presented centre
 * as 0xFFRRGGBB, or 0 before presentation/when diagnostics were disabled. */
u32  rsx_vulkan_backend_readback_center(void);

/* Draws of the last presented frame that ran the guest's own programs. */
u32  rsx_vulkan_backend_guest_draws(void);

/* 1 when draws honour the depth bounds test: the draw engine drives the
 * backend and the device has the depthBounds feature (lavapipe in Mesa 25
 * does not). Valid after init. */
int  rsx_vulkan_backend_depth_bounds(void);

/* 1 when draws run the guest's own programs: the HLSL -> SPIR-V translator
 * was built (glslang found) and PS3RECOMP_VK_GUEST_PROGRAMS=1 opts in. Then
 * the shared register-file draw engine (rsx_draw_engine.h) is the default
 * path, as it is Metal's; PS3RECOMP_RSX_ENGINE=vtable selects the older
 * rsx_state path instead. Opt-in while the path grows; needs no initialised
 * backend. */
int  rsx_vulkan_backend_guest_programs(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VULKAN_BACKEND_H */

/*
 * ps3recomp - host window sizing shared by the Win32 RSX backends
 *
 * The display (window client / swapchain) size is a host preference and is
 * deliberately separate from the guest's surfaces and from the internal
 * render size: a backend presents its finished frame into whatever client
 * area this produces, letterboxed by ps3_host_fit_rect.
 *
 * Everything here runs on the window's own thread. Requests from a settings
 * thread arrive only through the host_controls atomics.
 */
#ifndef PS3RECOMP_RSX_HOST_WINDOW_H
#define PS3RECOMP_RSX_HOST_WINDOW_H
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_host_window {
    HWND            hwnd;
    unsigned        default_w, default_h;  /* client size with no preference */
    unsigned        applied_w, applied_h;  /* last window-size request applied */
    int             fullscreen;            /* borderless state actually applied */
    LONG_PTR        style;                 /* windowed style, kept while borderless */
    WINDOWPLACEMENT placement;             /* windowed placement, ditto */
    volatile LONG   resized;               /* WM_SIZE seen since the last take */
} rsx_host_window;

/* Create the window: class `cls` must already be registered. The client
 * area is the host_controls window size (or default_w x default_h), in
 * physical pixels, fitted to the work area of the primary monitor. The
 * window is per-monitor DPI aware unless RSX_PIXEL_WINDOW=0, so a 1920x1080
 * request is 1920x1080 device pixels on a 150% display too. */
HWND rsx_host_window_create(rsx_host_window* w, const char* cls, const char* title,
                            unsigned default_w, unsigned default_h, DWORD extra_style);

/* WM_GETMINMAXINFO, WM_DPICHANGED and WM_SIZE. Returns 1 and sets *result
 * when the message was handled and must not reach DefWindowProc. */
int rsx_host_window_message(rsx_host_window* w, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

/* Poll the fullscreen flag and window size request; apply any change. */
void rsx_host_window_update(rsx_host_window* w);

/* Current client size; 0 when the window is minimized or has no area, in
 * which case a backend skips presenting and keeps its swapchain. */
int rsx_host_window_client(const rsx_host_window* w, unsigned* width, unsigned* height);

/* Nonzero once after any WM_SIZE: the swapchain should be checked. */
int rsx_host_window_take_resized(rsx_host_window* w);

/* Run code that measures the window (an overlay's ImGui frame, say) on a
 * thread other than the window's: give that thread the window's DPI
 * context for the duration, or its client rect is virtualized. */
void* rsx_host_window_dpi_enter(void);
void rsx_host_window_dpi_leave(void* previous);

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
#endif /* PS3RECOMP_RSX_HOST_WINDOW_H */

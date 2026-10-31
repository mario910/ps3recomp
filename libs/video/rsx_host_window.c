/*
 * ps3recomp - host window sizing shared by the Win32 RSX backends
 * See rsx_host_window.h.
 */
#ifdef _WIN32
#include "rsx_host_window.h"
#include "ps3emu/host_controls.h"
#include <stdio.h>
#include <stdlib.h>

/* Smallest client area a user can drag the window down to. */
#define MIN_CLIENT_W 320
#define MIN_CLIENT_H 180

typedef HANDLE (WINAPI *set_dpi_context_fn)(HANDLE);
typedef UINT (WINAPI *dpi_for_window_fn)(HWND);
typedef BOOL (WINAPI *adjust_for_dpi_fn)(LPRECT,DWORD,BOOL,DWORD,UINT);
static set_dpi_context_fn s_set_dpi;
static dpi_for_window_fn s_window_dpi;
static adjust_for_dpi_fn s_adjust_dpi;
static int s_pixel_window = -1;

static void load_dpi_api(void)
{
    if (s_pixel_window >= 0) return;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    s_set_dpi = (set_dpi_context_fn)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
    s_window_dpi = (dpi_for_window_fn)GetProcAddress(user32, "GetDpiForWindow");
    s_adjust_dpi = (adjust_for_dpi_fn)GetProcAddress(user32, "AdjustWindowRectExForDpi");
    /* Physical-pixel windows by default; RSX_PIXEL_WINDOW=0 keeps the old
     * DPI-unaware window that Windows bitmap-stretches. */
    const char* env = getenv("RSX_PIXEL_WINDOW");
    s_pixel_window = !(env && env[0] == '0') && s_set_dpi;
}

/* A window created per-monitor aware keeps that awareness, but calls made on
 * a thread with another DPI context see virtualized coordinates. Every call
 * here that measures or moves the window runs inside this bracket. */
static HANDLE dpi_enter(void)
{
    load_dpi_api();
    return s_pixel_window ? s_set_dpi((HANDLE)(INT_PTR)-4 /* PER_MONITOR_AWARE_V2 */) : NULL;
}
static void dpi_leave(HANDLE old) { if (old) s_set_dpi(old); }

static UINT window_dpi(HWND hwnd) { return s_window_dpi ? s_window_dpi(hwnd) : 96; }

/* Width and height the frame adds to a client area of this style. */
static void frame_extent(LONG_PTR style, UINT dpi, LONG* ex, LONG* ey)
{
    RECT r = {0, 0, 1000, 1000};
    if (s_adjust_dpi) s_adjust_dpi(&r, (DWORD)style, FALSE, 0, dpi);
    else AdjustWindowRect(&r, (DWORD)style, FALSE);
    *ex = (r.right - r.left) - 1000;
    *ey = (r.bottom - r.top) - 1000;
}

/* Shrink w x h, keeping its proportions, until it fits avail_w x avail_h. */
static void fit_client(unsigned* w, unsigned* h, LONG avail_w, LONG avail_h)
{
    if (avail_w < MIN_CLIENT_W) avail_w = MIN_CLIENT_W;
    if (avail_h < MIN_CLIENT_H) avail_h = MIN_CLIENT_H;
    if (*w <= (unsigned)avail_w && *h <= (unsigned)avail_h) return;
    const double sx = (double)avail_w / *w, sy = (double)avail_h / *h;
    const double s = sx < sy ? sx : sy;
    *w = (unsigned)(*w * s); *h = (unsigned)(*h * s);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static void requested_size(const rsx_host_window* w, unsigned* cw, unsigned* ch)
{
    ps3_host_get_window_size(cw, ch);
    if (!*cw || !*ch) { *cw = w->default_w; *ch = w->default_h; }
}

/* Size the windowed client area to cw x ch on the window's monitor, inside
 * its work area, moving the window only as far as it must to stay there. */
static void apply_windowed_size(rsx_host_window* w, unsigned cw, unsigned ch)
{
    MONITORINFO monitor = {sizeof(MONITORINFO)};
    if (!GetMonitorInfoA(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const RECT work = monitor.rcWork;
    LONG ex, ey;
    frame_extent(GetWindowLongPtrA(w->hwnd, GWL_STYLE), window_dpi(w->hwnd), &ex, &ey);
    const unsigned want_w = cw, want_h = ch;
    fit_client(&cw, &ch, (work.right - work.left) - ex, (work.bottom - work.top) - ey);
    RECT now;
    GetWindowRect(w->hwnd, &now);
    LONG x = now.left, y = now.top;
    const LONG ww = (LONG)cw + ex, wh = (LONG)ch + ey;
    if (x + ww > work.right) x = work.right - ww;
    if (y + wh > work.bottom) y = work.bottom - wh;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    SetWindowPos(w->hwnd, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    fprintf(stderr, "[host-controls] window client=%ux%u requested=%ux%u dpi=%u%s\n",
            cw, ch, want_w, want_h, window_dpi(w->hwnd),
            cw != want_w || ch != want_h ? " (fitted to the screen)" : "");
}

HWND rsx_host_window_create(rsx_host_window* w, const char* cls, const char* title,
                            unsigned default_w, unsigned default_h, DWORD extra_style)
{
    ZeroMemory(w, sizeof *w);
    w->default_w = default_w ? default_w : 1280;
    w->default_h = default_h ? default_h : 720;
    unsigned cw, ch;
    requested_size(w, &cw, &ch);
    HANDLE old = dpi_enter();
    const DWORD style = WS_OVERLAPPEDWINDOW | extra_style;
    /* Created hidden-sized; the real size needs the window's own DPI. */
    w->hwnd = CreateWindowExA(0, cls, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                              (int)cw, (int)ch, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (w->hwnd) {
        apply_windowed_size(w, cw, ch);
        w->applied_w = cw; w->applied_h = ch;
        RECT client = {0};
        GetClientRect(w->hwnd, &client);
        fprintf(stderr, "[window] guest=%ux%u client=%ldx%ld dpi=%u physical_pixels=%d\n",
                w->default_w, w->default_h, client.right, client.bottom,
                window_dpi(w->hwnd), s_pixel_window);
    }
    dpi_leave(old);
    return w->hwnd;
}

int rsx_host_window_message(rsx_host_window* w, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result)
{
    if (!w->hwnd) return 0;
    switch (msg) {
    case WM_GETMINMAXINFO: {
        LONG ex, ey;
        frame_extent(GetWindowLongPtrA(w->hwnd, GWL_STYLE), window_dpi(w->hwnd), &ex, &ey);
        MINMAXINFO* info = (MINMAXINFO*)lp;
        info->ptMinTrackSize.x = MIN_CLIENT_W + ex;
        info->ptMinTrackSize.y = MIN_CLIENT_H + ey;
        *result = 0;
        return 1;
    }
    case WM_DPICHANGED: {
        /* The setting is in device pixels: keep the client size, take only
         * the suggested position, and refit on the new monitor. Borderless
         * windows are refitted to their monitor by the next update. */
        if (!w->fullscreen) {
            const RECT* suggested = (const RECT*)lp;
            RECT client;
            GetClientRect(w->hwnd, &client);
            LONG ex, ey;
            frame_extent(GetWindowLongPtrA(w->hwnd, GWL_STYLE), HIWORD(wp), &ex, &ey);
            SetWindowPos(w->hwnd, NULL, suggested->left, suggested->top,
                         client.right + ex, client.bottom + ey, SWP_NOZORDER | SWP_NOACTIVATE);
            apply_windowed_size(w, (unsigned)client.right, (unsigned)client.bottom);
        }
        *result = 0;
        return 1;
    }
    case WM_SIZE:
        InterlockedExchange(&w->resized, 1);
        return 0;                       /* DefWindowProc still runs */
    }
    return 0;
}

void rsx_host_window_update(rsx_host_window* w)
{
    if (!w->hwnd) return;
    HANDLE old = dpi_enter();
    const int wanted = ps3_host_get_fullscreen();
    if (wanted >= 0 && wanted != w->fullscreen) {
        if (wanted) {
            MONITORINFO monitor = {sizeof(MONITORINFO)};
            w->placement.length = sizeof(WINDOWPLACEMENT);
            if (GetWindowPlacement(w->hwnd, &w->placement) &&
                GetMonitorInfoA(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                w->style = GetWindowLongPtrA(w->hwnd, GWL_STYLE);
                SetWindowLongPtrA(w->hwnd, GWL_STYLE, w->style & ~WS_OVERLAPPEDWINDOW);
                /* The swapchain follows the window: desktop resolution. */
                SetWindowPos(w->hwnd, NULL, monitor.rcMonitor.left, monitor.rcMonitor.top,
                             monitor.rcMonitor.right - monitor.rcMonitor.left,
                             monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                             SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
                w->fullscreen = 1;
                fprintf(stderr, "[host-controls] borderless-fullscreen=1 display=%ldx%ld\n",
                        monitor.rcMonitor.right - monitor.rcMonitor.left,
                        monitor.rcMonitor.bottom - monitor.rcMonitor.top);
            }
        } else {
            SetWindowLongPtrA(w->hwnd, GWL_STYLE, w->style);
            w->placement.showCmd = SW_SHOWNOACTIVATE;
            SetWindowPlacement(w->hwnd, &w->placement);
            SetWindowPos(w->hwnd, NULL, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            w->fullscreen = 0;
            fprintf(stderr, "[host-controls] borderless-fullscreen=0\n");
        }
    }
    /* A new window size applies to the windowed mode only. While borderless
     * or minimized it waits; the user's own resizing is never undone. */
    unsigned cw, ch;
    requested_size(w, &cw, &ch);
    if (!w->fullscreen && !IsIconic(w->hwnd) && (cw != w->applied_w || ch != w->applied_h)) {
        if (IsZoomed(w->hwnd)) ShowWindow(w->hwnd, SW_RESTORE);
        apply_windowed_size(w, cw, ch);
        w->applied_w = cw; w->applied_h = ch;
    }
    dpi_leave(old);
}

int rsx_host_window_client(const rsx_host_window* w, unsigned* width, unsigned* height)
{
    *width = *height = 0;
    if (!w->hwnd || IsIconic(w->hwnd)) return 0;
    HANDLE old = dpi_enter();
    RECT client = {0};
    const BOOL ok = GetClientRect(w->hwnd, &client);
    dpi_leave(old);
    if (!ok || client.right <= 0 || client.bottom <= 0) return 0;
    *width = (unsigned)client.right;
    *height = (unsigned)client.bottom;
    return 1;
}

void* rsx_host_window_dpi_enter(void) { return dpi_enter(); }
void rsx_host_window_dpi_leave(void* previous) { dpi_leave((HANDLE)previous); }

int rsx_host_window_take_resized(rsx_host_window* w)
{
    return InterlockedExchange(&w->resized, 0) != 0;
}
#endif /* _WIN32 */

#include "ps3emu/host_overlay.h"
static ps3_host_overlay_hooks s_hooks;
void ps3_host_set_overlay(const ps3_host_overlay_hooks* h) {
    if(h)s_hooks=*h;
    else {s_hooks.message=0;s_hooks.draw=0;s_hooks.shutdown=0;}
}
int ps3_host_overlay_message(void* w,unsigned m,uintptr_t a,intptr_t b) {
    return s_hooks.message?s_hooks.message(w,m,a,b):0;
}
void ps3_host_overlay_draw(const ps3_host_overlay_frame* f) {if(s_hooks.draw)s_hooks.draw(f);}
int ps3_host_overlay_enabled(void){return s_hooks.draw!=0;}
void ps3_host_overlay_shutdown(void) {if(s_hooks.shutdown)s_hooks.shutdown();}

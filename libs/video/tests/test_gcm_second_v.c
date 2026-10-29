/* The real deferred pump, not a model of it. Link with section GC so this
 * CPU-only test does not need a renderer or a game executable. */
#include "../cellGcmSys.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a;(void)v;(void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a;return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
static unsigned second_calls,vblank_calls,user_calls;
static int reenter;
static void texture_display_alias_checks(void)
{
    const u32 offset=0x01230000, address=0xc1230000;
    s_config.localAddress=0xc0000000;
    memset(s_display_buffer_set,0,sizeof(s_display_buffer_set));
    assert(!cellGcmTextureIsDisplay(offset,address,1280,720,5120));
    s_display_buffers[0]=(CellGcmDisplayInfo){offset,5120,1280,720};
    s_display_buffer_set[0]=1;
    assert(cellGcmTextureIsDisplay(offset,address,1280,720,5120));
    assert(!cellGcmTextureIsDisplay(offset,0x71230000,1280,720,5120));
    assert(!cellGcmTextureIsDisplay(0x0b610000,0xcb610000,1280,720,5120));
    assert(!cellGcmTextureIsDisplay(offset,address,1280,704,5120));
    assert(!cellGcmTextureIsDisplay(offset,address,1280,720,10240));
    assert(!cellGcmTextureIsDisplay(offset,address,1280,720,0));
    s_display_buffers[0].offset=0x81230000;
    assert(!cellGcmTextureIsDisplay(0x81230000,0x41230000,1280,720,5120));
    memset(s_display_buffer_set,0,sizeof(s_display_buffer_set));
    puts("Display texture alias checks passed: exact local address and layout, not dimensions");
}
static void callback(uint32_t opd,uint64_t a,uint64_t b,uint64_t c,
                     uint64_t d,uint64_t e,uint64_t f,uint64_t h,uint64_t i)
{
    assert(!b && !c && !d && !e && !f && !h && !i);
    if(opd==0x100) {
        assert(a==1);++second_calls;
        if(reenter) {reenter=0;cellGcmTickVBlank();ppu_gcm_pump();}
    } else if(opd==0x200) ++vblank_calls;
    else {assert(opd==0x300 && a==7);++user_calls;}
}
int main(void)
{
    texture_display_alias_checks();
    ppu_vm_size=0x10000;vm_base=calloc(1,ppu_vm_size);assert(vm_base);
    g_ps3_guest_caller=callback;
    if(!gcm_second_v_enabled()) {
        cellGcmSetSecondVHandler((CellGcmSecondVHandler)(uintptr_t)0x100);
        cellGcmSetVBlankHandler((CellGcmVBlankHandler)(uintptr_t)0x200);
        cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)0x300);
        cellGcmTickVBlank();cellGcmQueueUserCommand(7);ppu_gcm_pump();
        assert(!second_calls && vblank_calls==1 && user_calls==1);
        free(vm_base);puts("Second-V explicit isolation leaves other callbacks intact");return 0;
    }
    cellGcmSetSecondVFrequency(2);
    cellGcmSetSecondVHandler((CellGcmSecondVHandler)(uintptr_t)0x100);
    ppu_gcm_pump();assert(second_calls==0);
    cellGcmTickVBlank();assert(second_calls==0); /* Deferred, not ticker-side. */
    ppu_gcm_pump();assert(second_calls==1);
    ppu_gcm_pump();assert(second_calls==1); /* Exactly once per claimed tick. */
    for(unsigned n=0;n<10;++n)cellGcmTickVBlank();
    ppu_gcm_pump();assert(second_calls==2); /* Pending causes coalesce. */
    cellGcmSetSecondVFrequency(4);
    cellGcmTickVBlank();ppu_gcm_pump();assert(second_calls==2);
    cellGcmSetSecondVFrequency(3);
    cellGcmTickVBlank();ppu_gcm_pump();assert(second_calls==3);
    cellGcmSetSecondVHandler(NULL);
    cellGcmTickVBlank();ppu_gcm_pump();assert(second_calls==3);
    cellGcmSetSecondVHandler((CellGcmSecondVHandler)(uintptr_t)0x100);
    cellGcmSetVBlankHandler((CellGcmVBlankHandler)(uintptr_t)0x200);
    cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)0x300);
    reenter=1;cellGcmTickVBlank();cellGcmQueueUserCommand(7);
    ppu_gcm_pump();
    assert(second_calls==4 && vblank_calls==1 && user_calls==1);
    ppu_gcm_pump();assert(second_calls==5 && vblank_calls==2 && user_calls==1);
    /* No stale callback survives termination and a fresh tick. */
    cellGcmSetVBlankHandler(NULL);cellGcmSetUserHandler(NULL);
    cellGcmTerminate();cellGcmTickVBlank();ppu_gcm_pump();
    assert(second_calls==5);
    free(vm_base);vm_base=NULL;
    puts("Second-V deferred delivery checks passed");
}

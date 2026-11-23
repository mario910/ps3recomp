/* Exercise the production parked-list acknowledgement path. Only external
 * observation/coherence dependencies are inert; DMA list parsing and copies
 * come directly from spu_dma.h. Each process starts a fresh diagnostic cap. */
#ifndef _WIN32
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../spu_dma.h"

static uint8_t memory[0x40000];
uint8_t* vm_base = memory;
uint8_t g_vm_page_bitmap[65536 / 8];
int g_cri_video_dma;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_barrier_sync_watch;
#ifdef _WIN32
/* This DMA fixture has no PPU usleep wait sites. */
volatile uint32_t g_ppu_wake_ea[2];
volatile long g_ppu_wake_count_dma;
#endif
void (*g_spu_put_hook)(uint32_t, uint32_t);
void (*g_spu_put_notify)(uint32_t, uint32_t);
void (*g_spu_dma_observer)(spu_context*, uint32_t, uint32_t, uint32_t, uint32_t, const void*);
SPU_THREAD_LOCAL unsigned g_spu_run_serial;
void* volatile g_sn_defer_ctx;
volatile unsigned g_sn_defer;
unsigned long long ps3_ms_now(void) { return 0; }
unsigned spu_sn_defer_ticks(void) { return 0; }
unsigned* spu_ls_watch_list(int* n) { *n=0;return NULL; }
int g_spu_ls_watch_n;
uint32_t g_ydkj_real_taskset_ea;
SPU_THREAD_LOCAL volatile unsigned g_wws_batch_gets;
void spu_halt(spu_context* ctx) { (void)ctx; abort(); }

static mfc_engine* observed_engine;
static unsigned callback_step;
static void check(int ok, const char* what);
static void observe_get(spu_context* ctx, uint32_t dir, uint32_t lsa,
                        uint32_t ea, uint32_t size, const void* data)
{
    check(callback_step++ == 0 && dir == 1, "GET observer order");
    check(lsa == ctx->mfc_lsa && ea == ctx->mfc_eal && size == ctx->mfc_size,
          "observer range");
    check(data == ctx->ls + lsa && !memcmp(data, memory + ea, size), "observer payload");
    check(!(observed_engine->tag_completed & (1u << ctx->mfc_tag)), "tag busy in callback");
}
void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* data, uint32_t size)
{
    if (!observed_engine) return;
    check(callback_step++ == 1 && ea == ctx->mfc_eal && size == ctx->mfc_size &&
          data == ctx->ls + ctx->mfc_lsa, "overlay follows observer for the same GET");
}
int spu_coh_span_reserved(uint32_t first, uint32_t last) { (void)first; (void)last; return 0; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea;return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea,const void* c) { (void)ea;(void)c; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockguard_check(spu_context* c,uint32_t ea,const uint8_t* p,uint32_t n,const char* what)
{ (void)c;(void)ea;(void)p;(void)n;(void)what; }
void spu_ch_wake(spu_context* c) { (void)c; }

static void check(int ok,const char* what)
{ if(!ok){fprintf(stdout,"FAIL: %s\n",what);exit(2);} }
static int exercise(unsigned worker,unsigned iteration,int mode)
{
    spu_context* ctx=(spu_context*)calloc(1,sizeof(*ctx));
    check(ctx!=NULL,"allocate context");spu_context_init(ctx,worker);
    mfc_engine mfc;mfc_engine_init(&mfc);
    const uint32_t tag=5,bit=1u<<tag,other=1u<<31,ea=0x10000+worker*0x100;
    uint8_t pattern[32];for(unsigned i=0;i<32;i++)pattern[i]=(uint8_t)(iteration*3+i);
    memcpy(ctx->ls+0x1000,pattern,32);memset(memory+ea,0xA5,32);
    ctx->list_stall_mask=other;mfc.tag_completed&=~bit;
    spu_ls_write32(ctx,0x100,0x80000000u);spu_ls_write32(ctx,0x104,0);
    unsigned count=1;
    if(mode==1){
        count=2;spu_ls_write32(ctx,0x108,16);spu_ls_write32(ctx,0x10C,ea);
        if(iteration&1){memcpy(memory+ea,pattern,16);memset(ctx->ls+0x1000,0xCC,16);}
    }else if(mode==2){
        count=3;spu_ls_write32(ctx,0x108,0x80000010u);spu_ls_write32(ctx,0x10C,ea);
        spu_ls_write32(ctx,0x110,16);spu_ls_write32(ctx,0x114,ea+16);
    }
    const uint32_t cmd=mode==1 && (iteration&1)?0x40:0x20;
    check(mfc_run_list(ctx,0x100,count,0x1000,0,cmd,tag)==0,"initial list return");
    check(ctx->list_stall_mask==(other|bit),"real initial stall");
    check((ctx->event_status&2)!=0,"stall event raised");
    check(mfc_list_stall_ack(&mfc,ctx,tag)==0,"ack return");
    if(mode==2){
        check(ctx->list_stall_mask==(other|bit),"repark retains active tag");
        check(!(mfc.tag_completed&bit),"repark does not complete tag");
        check(ctx->list_stall_remaining[tag]==1,"repark remaining element");
        check(ctx->list_stall_dest_lsa[tag]==0x1010,"repark destination cursor");
        check(memcmp(memory+ea,pattern,16)==0,"first repark PUT bytes");
        check(memory[ea+16]==0xA5,"unexecuted second element");
        check(mfc_list_stall_ack(&mfc,ctx,tag)==0,"second ack return");
    }
    check(ctx->list_stall_mask==other,"ack clears only selected parked tag");
    check((mfc.tag_completed&bit)!=0,"drained tag completes");
    check(mfc.queue_count==0,"queue state preserved");
    if(mode==0){check(memcmp(ctx->ls+0x1000,pattern,32)==0,"empty LS unchanged");check(memory[ea]==0xA5,"empty VM unchanged");}
    if(mode==1){check(memcmp(ctx->ls+0x1000,pattern,16)==0,"GET/PUT LS bytes");check(memcmp(memory+ea,pattern,16)==0,"GET/PUT VM bytes");}
    if(mode==2)check(memcmp(memory+ea,pattern,32)==0,"both resumed PUT elements");
    free(ctx);return 0;
}
static void exercise_get_channels(void)
{
    spu_context* ctx = calloc(1, sizeof(*ctx));
    check(ctx != NULL, "allocate GET context");
    spu_context_init(ctx, 0);
    mfc_engine engine;
    mfc_engine_init(&engine);
    observed_engine = &engine;
    g_spu_dma_observer = observe_get;
    const unsigned sizes[] = {8, 32, 512};
    const uint32_t ea = 0x1FFF0, lsa = 0xFF0, tag = 5, bit = 1u << tag;
    for (unsigned i = 0; i < 512; ++i) memory[ea+i] = (uint8_t)(13*i+1);
    for (unsigned cmd = MFC_GET_CMD; cmd <= MFC_GETF_CMD; ++cmd) {
        for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
            for (unsigned state = 0; state < 6; ++state) {
                ctx->image_id = (state & 1) ? 22 : 39;
                ctx->list_stall_mask = state < 2 ? 0 : state < 4 ? bit : 1u << 31;
                engine.tag_completed = ~ctx->list_stall_mask;
                g_cri_video_dma = 0;
                callback_step = 0;
                memset(ctx->ls+lsa-1, 0xA5, sizes[i]+2);
                mfc_channel_write(&engine, ctx, MFC_LSA, lsa);
                mfc_channel_write(&engine, ctx, MFC_EAL, ea);
                mfc_channel_write(&engine, ctx, MFC_Size, sizes[i]);
                mfc_channel_write(&engine, ctx, MFC_TagID, tag);
                mfc_channel_write(&engine, ctx, MFC_Cmd, cmd);
                check(callback_step == 2 && !memcmp(ctx->ls+lsa, memory+ea, sizes[i]),
                      "GET delivered once");
                check(ctx->ls[lsa-1] == 0xA5 && ctx->ls[lsa+sizes[i]] == 0xA5, "GET bounds");
                check(engine.tag_completed == ~ctx->list_stall_mask, "parked tags preserved");
                check(g_cri_video_dma == (ctx->image_id == 22 && sizes[i] > 256), "video DMA flag");
            }
        }
    }
    /* Both the lean plain command and the ordered forms must reject this
     * descriptor before calling either observer or touching the LS. */
    ctx->image_id = 39;
    ctx->list_stall_mask = 0;
    ctx->mfc_lsa = lsa+1;
    ctx->mfc_size = 32;
    memset(ctx->ls+lsa, 0xA5, 33);
    for (unsigned cmd = MFC_GET_CMD; cmd <= MFC_GETF_CMD; ++cmd) {
        callback_step = 0;
        mfc_channel_write(&engine, ctx, MFC_Cmd, cmd);
        check(callback_step == 0, "invalid descriptor has no callbacks");
        for (unsigned i = 0; i < 33; ++i) check(ctx->ls[lsa+i] == 0xA5, "invalid GET bounds");
    }
    g_spu_dma_observer = NULL;
    observed_engine = NULL;

    /* The first GET replaces the second descriptor in LS. The runner must
     * decode that new descriptor before copying the second element. */
    const uint8_t next_desc[16] = {0, 0, 0, 16, 0, 1, 0x10, 0x10};
    memcpy(memory + 0x11000, next_desc, 16);
    memset(memory + 0x11010, 0x79, 16);
    memset(ctx->ls + 0x1000, 0xCC, 32);
    spu_ls_write32(ctx, 0xFF8, 16);
    spu_ls_write32(ctx, 0xFFC, 0x11000);
    /* The original descriptor is empty, including during the prefetch pass. */
    spu_ls_write32(ctx, 0x1000, 0);
    spu_ls_write32(ctx, 0x1004, 0);
    check(mfc_run_list(ctx, 0xFF8, 2, 0x1000, 0, MFC_GET_CMD, tag) == 0, "overlapping list");
    check(!memcmp(ctx->ls + 0x1000, next_desc, 16), "replaced descriptor");
    check(!memcmp(ctx->ls + 0x1010, memory + 0x11010, 16), "new descriptor consumed");
    free(ctx);
}

static volatile LONG arrived,go;
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID arg)
#else
static void* worker(void* arg)
#endif
{
    unsigned n=(unsigned)(uintptr_t)arg;InterlockedIncrement(&arrived);
    while(!InterlockedCompareExchange(&go,0,0))YieldProcessor();
    for(unsigned i=0;i<64;i++)exercise(n,i,0);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
int main(int argc,char** argv)
{
    check(argc==2,"mode argument");memset(g_vm_page_bitmap,0xFF,sizeof g_vm_page_bitmap);
    if (strcmp(argv[1], "channels") == 0) {
        exercise_get_channels();
        puts("OK: ordered GET channels, observers, parked tags and video flag");
        return 0;
    }
    if(strcmp(argv[1],"concurrent")==0){
#ifdef _WIN32
        HANDLE th[8];for(unsigned i=0;i<8;i++){th[i]=CreateThread(NULL,0,worker,(LPVOID)(uintptr_t)i,0,NULL);check(th[i]!=NULL,"start worker");}
#else
        pthread_t th[8];for(unsigned i=0;i<8;i++)check(pthread_create(&th[i],NULL,worker,(void*)(uintptr_t)i)==0,"start worker");
#endif
        while(InterlockedCompareExchange(&arrived,0,0)!=8)YieldProcessor();InterlockedExchange(&go,1);
        for(unsigned i=0;i<8;i++){
#ifdef _WIN32
            check(WaitForSingleObject(th[i],30000)==WAIT_OBJECT_0,"join worker");CloseHandle(th[i]);
#else
            check(pthread_join(th[i],NULL)==0,"join worker");
#endif
        }
    }else{
        int mode=strcmp(argv[1],"transfer")==0?1:strcmp(argv[1],"repark")==0?2:0;
        for(unsigned i=0;i<64;i++)exercise(0,i,mode);
    }
    puts("OK: actual parked/resume transfers, masks, completions and returns");return 0;
}

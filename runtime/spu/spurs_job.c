/* spurs_job.c — stage and enter a SPURS jobchain job the way Sony's jm2 does.
 *
 * A job binary is not an SPU ELF and not a policy module: it is a raw image
 * built by the SDK's job_elf-to-bin, linked against Sony's job_start_w_crt.o.
 * That CRT is what every job entry actually runs first — it saves r3/r4, runs
 * the ctors, and calls
 *
 *     void cellSpursJobMain2(CellSpursJobContext2 *jobContext,   // r3
 *                            CellSpursJob256      *job);         // r4
 *
 * (target/spu/include/cell/spurs/job_chain.h). Both are LOCAL STORE pointers,
 * so the manager must have laid the whole working set out in LS before entry.
 * Entering a job without that context is what left LBP's job parked with zero
 * DMA traffic: it reads its ioBuffer pointer out of a zeroed context and stalls.
 *
 * The layout is pinned by _cellSpursCheckJob() in
 * target/common/include/cell/spurs/job_descriptor.h, which sums the exact LS a
 * job consumes. For a jobchain it runs with align = 1024, isScratchStackUnified
 * = 1, maxUsableMemorySize = CELL_SPURS_MAX_SIZE_JOB_MEMORY (234 KB):
 *
 *     align1024(sizeBinary<<4)                                   binary + bss
 *   + align1024(sizeInOrInOut)                                   ioBuffer
 *   + align1024(sizeOut)                                         oBuffer
 *   + align1024((sizeScratch<<4) + (sizeStack ? sizeStack<<4 : 8192))
 *                                                                scratch + stack
 *   + per cache entry: align1024(size)                           cacheBuffer[0..3]
 *
 * We reproduce that arithmetic rather than invent a layout, so a job that
 * range-checks its own buffers sees what it would on hardware.
 *
 * Load address: the job binary is position-independent (job_start_w_crt derives
 * its load delta with a brsl-to-next / subtract-link-time-address idiom), so jm2
 * may place it anywhere. We place it at LS 0 — the lifted C is keyed to
 * link-time addresses, which makes the delta zero and the lifted branch targets
 * exact.
 */

#include "spu_coherency.h"
#include "spu_workload.h"
#include "spu_context.h"
#include "spurs_job_ls.h"
#include "spurs_job_options.h"
#if defined(SPU_NATIVE_RTZ_IMAGE) || defined(SPU_NATIVE_RTZ_EXTRA_IMAGE) || defined(SPU_NATIVE_RTZ_IMAGE39)
#include <immintrin.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#endif
#include "ps3emu/log_first.h"
#include "ps3emu/ps3_ftrace.h"
#ifndef _WIN32
#include "../platform/win32_compat.h"
#endif

extern uint8_t* vm_base;
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);

/* Capture short jobs without also tracing millions of sub-microsecond HLE
 * calls. This affects diagnostics only, and is immutable after startup. */
static int job_trace_all;
__attribute__((constructor)) static void job_trace_options_init(void)
{
    const char* all = getenv("SPURS_JOB_TRACE_ALL");
    job_trace_all = all && all[0] == '1' && all[1] == '\0';
}

#ifdef SPU_NATIVE_RTZ_EXTRA_IMAGE
#include "spu_native_extra_fp.h"
int spu_native_extra_fp_mode;
__attribute__((constructor)) static void native_extra_fp_init(void)
{
    const char* mode = getenv("SPURS_NATIVE_EXTRA_FP");
    /* Reject malformed values; never change the environment for mode 0. */
    if (mode && (mode[0] == '1' || mode[0] == '2') && mode[1] == '\0')
        spu_native_extra_fp_mode = mode[0] - '0';
}
#endif

#ifdef SPU_NATIVE_RTZ_IMAGE39
#include "spu_native_extra_fp.h"
int spu_native_image39_fp_mode;
__attribute__((constructor)) static void native_image39_fp_init(void)
{
    const char* mode = getenv("SPURS_NATIVE_IMAGE39_FP");
    if (mode && (mode[0] == '1' || mode[0] == '2') && mode[1] == '\0')
        spu_native_image39_fp_mode = mode[0] - '0';
}
#endif

/* ---- guest reads (big-endian) ------------------------------------------ */
static uint32_t g32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint16_t g16(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}
static uint64_t g64(uint32_t ea)
{
    return ((uint64_t)g32(ea) << 32) | (uint64_t)g32(ea + 4);
}

/* ---- local-store writes (big-endian) ----------------------------------- */
static void ls32(uint8_t* ls, uint32_t o, uint32_t v)
{
    ls[o] = (uint8_t)(v >> 24); ls[o+1] = (uint8_t)(v >> 16);
    ls[o+2] = (uint8_t)(v >> 8); ls[o+3] = (uint8_t)v;
}
static void ls16(uint8_t* ls, uint32_t o, uint16_t v)
{
    ls[o] = (uint8_t)(v >> 8); ls[o+1] = (uint8_t)v;
}

/* CellSpursJobHeader, 48 B — job_descriptor.h */
enum {
    JH_EA_BINARY           = 0x00,   /* be u64; low bit = CACHE_INHIBIT flag */
    JH_SIZE_BINARY         = 0x08,   /* be u16; bytes >> 4                   */
    JH_SIZE_DMA_LIST       = 0x0A,   /* be u16; input list size in BYTES     */
    JH_EA_HIGH_INPUT       = 0x0C,
    JH_USE_INOUT_BUFFER    = 0x10,
    JH_SIZE_IN_OR_INOUT    = 0x14,
    JH_SIZE_OUT            = 0x18,
    JH_SIZE_STACK          = 0x1C,   /* be u16; quadwords */
    JH_SIZE_SCRATCH        = 0x1E,   /* be u16; quadwords */
    JH_EA_HIGH_CACHE       = 0x20,
    JH_SIZE_CACHE_DMA_LIST = 0x24,   /* be u32; bytes */
    JH_JOB_TYPE            = 0x2C,
    JH_SIZE                = 0x30,
};

/* CellSpursJobContext2, 48 B — job_context_types.h. SPU pointers are 32-bit LS
 * offsets and the struct lives in local store, so every field is big-endian. */
enum {
    JC_IO_BUFFER          = 0x00,
    JC_CACHE_BUFFER       = 0x04,   /* void *cacheBuffer[4] */
    JC_SIZE_JOB_DESC      = 0x14,   /* sizeJobDescriptor:4 + pad:28 */
    JC_NUM_IO_BUFFER      = 0x18,
    JC_NUM_CACHE_BUFFER   = 0x1A,
    JC_O_BUFFER           = 0x1C,
    JC_S_BUFFER           = 0x20,
    JC_DMA_TAG            = 0x24,
    JC_EA_JOB_DESCRIPTOR  = 0x28,   /* be u64 */
    JC_SIZE               = 0x30,
};

#define ALIGN1024(x)  (((x) + 1023u) & ~1023u)
#define JOB_DMA_TAG   20u          /* "one of: {20,21}" — job_context_types.h */
#define JOB_STACK_DEFAULT 8192u    /* _cellSpursCheckJob's sizeStack==0 default */

/* Local store of the most recent job, kept alive past the run.
 *
 * A title can poll the SPU that runs a job chain with sys_spu_thread_read_LS,
 * identifying it by the CHAIN HANDLE rather than an lv2 thread id -- Tokyo
 * Jungle reads its bus counts that way, 45k polls of offset 0 in a 45 s run.
 * The job context is a stack local, so by the time the PPU looks there is
 * nothing to read and every poll fails. Retain the store and let the syscall
 * resolve a chain handle to it. */
/* Storage and copying are chain-owned in spurs_job_ls.c, not one global LS
 * with a separately raced handle written by unrelated chain walkers. */

/* Last outbound mailbox posted by a finished job (see below). */
/* THREAD-LOCAL. A job's answer is read back by the completion event that
 * follows it, and jc_execute signals immediately after each job -- but the walk
 * runs on whichever guest thread drives the chain, and more than one can be in
 * flight. As plain globals these were clobbered between a job finishing and its
 * own completion being pushed, so a query could be answered with an unrelated
 * job's mailbox. Per-thread keeps each job paired with its own answer. */
/* __declspec(thread) for MSVC only: MinGW GCC ignores it, which would make
 * these process-wide. spurs_job.c and cellSpurs.c must agree. */
#if defined(_MSC_VER)
#  define SPURS_JOB_TLS __declspec(thread)
#else
#  define SPURS_JOB_TLS __thread
#endif
SPURS_JOB_TLS uint32_t g_spurs_job_mbox, g_spurs_job_mbox_intr;
/* The command this job ran, read from its own command block. Only a query
 * expects an answer back through the completion event. */
SPURS_JOB_TLS uint32_t g_spurs_job_cmd;
SPURS_JOB_TLS int g_spurs_job_mbox_valid;
/* The SPU number a job sees (cellSpursGetCurrentSpuId, kernel LS 0x1C8). A host
 * thread that runs jobs concurrently with others must claim its own: titles
 * key per-SPU resources on it (Edge output rings, one RSX label per SPU), and
 * two jobs that both think they are SPU 0 corrupt each other's ring. */
SPURS_JOB_TLS uint32_t g_spurs_job_spu_num;
static SPURS_JOB_TLS uint32_t s_job_owner;
void spurs_job_set_owner(uint32_t chain_ea) { s_job_owner=chain_ea; }
void spurs_job_set_spu_num(uint32_t n) { g_spurs_job_spu_num = n; }
uint32_t spurs_job_get_spu_num(void) { return g_spurs_job_spu_num; }

static SPURS_JOB_TLS spu_context* s_spare_context;
void spurs_job_release_thread_context(void)
{
#ifdef _WIN32
    _aligned_free(s_spare_context);
#else
    free(s_spare_context);
#endif
    s_spare_context = NULL;
}

static double job_clock(void)
{
    struct timespec t;timespec_get(&t,TIME_UTC);
    return t.tv_sec+t.tv_nsec*1e-9;
}
static void job_phase_report(int image,double start,double enter,double leave,double end,int seconds,unsigned used_mfc,unsigned used_atomic)
{
    struct Phase {unsigned n,mfc,atomic;double setup,run,post;};
    static SPURS_JOB_TLS struct Phase stats[64];
    static SPURS_JOB_TLS double last;
    if(image>=0 && image<64){
        struct Phase* p=&stats[image];p->n++;p->mfc+=used_mfc;p->atomic+=used_atomic;
        p->setup+=enter-start;p->run+=leave-enter;p->post+=end-leave;
    }
    if(!last)last=end;
    if(end-last<seconds)return;
    for(unsigned i=0;i<64;++i)if(stats[i].n){
        struct Phase* p=&stats[i];
        fprintf(stderr,"[jobphase] image=%u n=%u setup=%.3fs run=%.3fs post=%.3fs average=%.1fus mfc=%u atomic=%u\n",
                i,p->n,p->setup,p->run,p->post,(p->setup+p->run+p->post)*1e6/p->n,p->mfc,p->atomic);
    }
    memset(stats,0,sizeof stats);last=end;
}

/* Job corpus hooks (port/job_corpus.cpp): called on the worker thread right
 * before and after the lifted entry runs, with the fully staged context. */
void (*g_spurs_job_capture_begin)(spu_context*, int, uint32_t) = 0;
void (*g_spurs_job_capture_end)(spu_context*, int, uint32_t) = 0;

static void spurs_job_enter(spu_lifted_entry_fn entry, int image_id, spu_context* ctx)
{
    (void)image_id;
#if defined(SPU_NATIVE_RTZ_IMAGE) || defined(SPU_NATIVE_RTZ_EXTRA_IMAGE) || defined(SPU_NATIVE_RTZ_IMAGE39)
    /* Only the configured, audited single-precision TUs use native RZ helpers.
     * Save/restore even on the job's HALT/longjmp completion path: that jump
     * is caught by spu_run_with_halt, not by this caller. Other images and
     * host/PPU math keep their incoming environment. */
    unsigned saved_mxcsr = _mm_getcsr();
    int native_rtz = 0;
#ifdef SPU_NATIVE_RTZ_IMAGE
    native_rtz = image_id == SPU_NATIVE_RTZ_IMAGE;
#endif
#ifdef SPU_NATIVE_RTZ_EXTRA_IMAGE
    native_rtz |= image_id == SPU_NATIVE_RTZ_EXTRA_IMAGE && spu_native_extra_fp_mode != 0;
#endif
#ifdef SPU_NATIVE_RTZ_IMAGE39
    native_rtz |= image_id == SPU_NATIVE_RTZ_IMAGE39 && spu_native_image39_fp_mode != 0;
#endif
    if (native_rtz)
        _mm_setcsr((saved_mxcsr & ~0x6000u) | 0x6000u | 0x8040u | 0x1f80u);
#endif
    spu_run_with_halt(entry, ctx);
#if defined(SPU_NATIVE_RTZ_IMAGE) || defined(SPU_NATIVE_RTZ_EXTRA_IMAGE) || defined(SPU_NATIVE_RTZ_IMAGE39)
    _mm_setcsr(saved_mxcsr);
#endif
}

/* Replay a captured job: ctx already holds the entry registers and LS. */
void spurs_job_run_prepared(spu_lifted_entry_fn entry, int image_id, spu_context* ctx)
{
    spurs_job_enter(entry, image_id, ctx);
    spu_mfc_release(ctx);
    spu_coh_unregister(ctx);
}

static int run_spurs_job_context(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size, spu_context* ctx, int exchange)
{
    if (!entry || !job_ea) return -1;
    const uint32_t job_owner = s_job_owner;
    const uint64_t trace_start = PS3_FT_ON() ? ps3_ft_now() : 0;
    static SPURS_JOB_TLS int phase_seconds=-1;
    if(phase_seconds<0){const char* e=getenv("SPURS_JOB_PHASEPROF");phase_seconds=e?atoi(e):0;}
    const double phase_start=phase_seconds?job_clock():0;
    static SPURS_JOB_TLS int cache_options = -1;
    static SPURS_JOB_TLS SpursJobOptions option_cache;
    if (cache_options < 0) {
        const char* mode = getenv("SPURS_JOB_CACHE_ENV");
        cache_options = mode && mode[0] && mode[0] != '0';
    }
    SpursJobOptions live_options;
    const SpursJobOptions* options = spurs_job_options_for_call(
        &option_cache, &live_options, cache_options);

    /* ---- descriptor ---------------------------------------------------- */
    uint32_t ea_bin   = (uint32_t)(g64(job_ea + JH_EA_BINARY) & ~1ull);
    uint32_t size_bin = (uint32_t)g16(job_ea + JH_SIZE_BINARY) << 4;
    uint32_t n_dma    = (uint32_t)g16(job_ea + JH_SIZE_DMA_LIST) / 8u;
    uint32_t use_io   = g32(job_ea + JH_USE_INOUT_BUFFER);
    uint32_t size_io  = g32(job_ea + JH_SIZE_IN_OR_INOUT);
    uint32_t size_out = g32(job_ea + JH_SIZE_OUT);
    uint32_t size_stk = (uint32_t)g16(job_ea + JH_SIZE_STACK) << 4;
    uint32_t size_scr = (uint32_t)g16(job_ea + JH_SIZE_SCRATCH) << 4;
    uint32_t n_cache  = g32(job_ea + JH_SIZE_CACHE_DMA_LIST) / 8u;

    if (n_cache > 4) n_cache = 4;                    /* jm2 caps at 4 */
    if (!size_stk) size_stk = JOB_STACK_DEFAULT;
    if (!size_bin || size_bin > SPU_LS_SIZE) {
        fprintf(stderr, "[spurs-job] job 0x%08X: bad sizeBinary=%u -- not run\n",
                job_ea, size_bin);
        return -1;
    }

    /* Stage directly into the job's own zeroed local store. The old path
     * calloc'ed a second 256 KiB store, then zeroed ctx and copied all of it
     * again. Per-job ownership and the guest bytes are unchanged. */
    spu_context_init(ctx,0);
    ctx->image_id=image_id;
    uint8_t* ls=ctx->ls;

    /* ---- binary at LS 0 (PIC, so delta 0 keeps the lifted addresses) ---- */
    memcpy(ls, vm_base + ea_bin, size_bin);

    /* ---- regions, following _cellSpursCheckJob's own arithmetic --------- */
    uint32_t p         = ALIGN1024(size_bin);
    /* Same rule as the scratch pointer: these are REGION BASES and stay valid
     * when the region is empty. NULL invites the job to dereference 0 -- and
     * since the job binary loads at LS 0, reading through a null base returns
     * the job's OWN INSTRUCTION WORDS. That is exactly what Tokyo Jungle's
     * wedged jobs were doing: each spun DMAing to the word at offset 0x64 of
     * its own image, which is why the addresses were misaligned and looked
     * like floats -- they were opcodes. */
    uint32_t io_ls     = p;                p += ALIGN1024(size_io);
    uint32_t out_ls    = p;                p += ALIGN1024(size_out);
    if (use_io && !out_ls) out_ls = io_ls;    /* in-out: output shares ioBuffer */
    uint32_t ss_size   = ALIGN1024(size_scr + size_stk);
    /* The scratch pointer is the BASE OF THE SCRATCH+STACK BLOCK, and it is
     * valid even when sizeScratch is 0 -- a zero-length buffer still has an
     * address. Handing the job a NULL here meant it dereferenced 0 and read
     * ITS OWN CODE as data: every wedged job in Tokyo Jungle was loading a
     * word from LS 0x64 and using that SPU instruction word as a DMA address,
     * which is why the addresses looked like misaligned floats. */
    uint32_t s_ls      = p;
    uint32_t stack_top = p + ss_size;
    p += ss_size;

    /* ---- cache-hinted read-only inputs ---------------------------------- */
    uint32_t dma_base   = job_ea + JH_SIZE;          /* input list ... */
    uint32_t cache_base = dma_base + n_dma * 8u;     /* ... then cache list */
    uint32_t cache_ls[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n_cache; i++) {
        uint64_t e   = g64(cache_base + i * 8u);
        uint32_t sz  = (uint32_t)(e >> 32);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (p + sz > SPU_LS_SIZE) break;
        cache_ls[i] = p;
        memcpy(ls + p, vm_base + eal, sz);
        p += ALIGN1024(sz);
    }

    /* ---- input DMA list -> ioBuffer, each entry 16-byte aligned ---------- */
    uint32_t io_cur = io_ls, io_used = 0;
    for (uint32_t i = 0; i < n_dma; i++) {
        uint64_t e   = g64(dma_base + i * 8u);
        uint32_t sz  = (uint32_t)((e >> 32) & 0x7FFFu);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (!io_ls || io_cur + sz > SPU_LS_SIZE) break;
        memcpy(ls + io_cur, vm_base + eal, sz);
        io_cur  += (sz + 15u) & ~15u;
        io_used += (sz + 15u) & ~15u;
    }

    /* ---- context + descriptor, parked above the job's memory region ------
     * jm2 keeps these in its own bss (outside job memory); we load the job at
     * LS 0, so the top of LS is the equivalent free space. */
    uint32_t desc_ls = SPU_LS_SIZE - 0x400;     /* descriptors are <= 1024 B */
    uint32_t ctx_ls  = desc_ls - 0x40;          /* CellSpursJobContext2, 48 B */
    uint32_t dsz     = job_desc_size ? job_desc_size : 256u;
    if (dsz > 0x400) dsz = 0x400;
    if (p > ctx_ls)
        fprintf(stderr, "[spurs-job] job 0x%08X: WARNING regions reach 0x%X, "
                        "past the context at 0x%X\n", job_ea, p, ctx_ls);
    memcpy(ls + desc_ls, vm_base + job_ea, dsz);

    ls32(ls, ctx_ls + JC_IO_BUFFER, io_ls);
    for (int i = 0; i < 4; i++)
        ls32(ls, ctx_ls + JC_CACHE_BUFFER + (uint32_t)i * 4u, cache_ls[i]);
    /* sizeJobDescriptor:4 is the first bitfield of a big-endian u32, so it
     * occupies the TOP 4 bits. 0 means 64 bytes, otherwise n*128. */
    /* The low 28 bits are not padding to a "JOBCRT Ver13" crt (WWS/Edge jobs,
     * Twisted Metal's JobQueue jobs): jm2 parks an LS pointer to its kernel
     * block there, and the crt returns 0x80410A11 and STOPs before main when
     * it is 0 -- so the job never runs and never clears its completion
     * counter. Hand it the free 16 bytes after the context (zeroed); the crt
     * only stashes the pointer and clears the field again. */
    uint32_t jm_iface = memcmp(ls + 0x30, "JOBCRT Ver13", 12) == 0 ? ctx_ls + 0x30 : 0;
    /* The same crt also reads the SPURS kernel context with ABSOLUTE loads
     * (lqa): spuNum at 0x1C8 (cellSpursGetCurrentSpuId), the trace buffer at
     * 0x210, a trace-enable byte at 0x175. On hardware the kernel owns
     * 0x100-0x2FF; here the job is loaded at 0, so those reads returned the
     * job's own instruction words and it indexed per-SPU tables with an opcode
     * and halted. The lifted code never reads those bytes as data, so overlay
     * the kernel's view: SPU 0, our DMA tag, tracing off. */
    if (jm_iface) {
        memset(ls + 0x170, 0, 0x10);
        memset(ls + 0x1C0, 0, 0x10);
        ls32(ls, 0x1C8, g_spurs_job_spu_num);
        ls32(ls, 0x1CC, JOB_DMA_TAG);
        memset(ls + 0x210, 0, 0x10);
    }
    /* PIC raw jobs call the same resident SPURS services as JOBCRT Ver13.
     * Their absolute LS 0x1C0 reads need the kernel's SPURS pointer and ID,
     * not the instruction bytes of a job we relocated to LS zero. */
    if (s_job_owner && options->kernel) {
        memset(ls + 0x1C0,0,16);
        ls32(ls,0x1C0,g32(s_job_owner+0x78));
        ls32(ls,0x1C4,g32(s_job_owner+0x7C));
        ls32(ls,0x1C8,g_spurs_job_spu_num);
        ls32(ls,0x1CC,JOB_DMA_TAG);
    }
    ls32(ls, ctx_ls + JC_SIZE_JOB_DESC,
         ((dsz == 64 ? 0u : (dsz / 128u) & 0xFu) << 28) | jm_iface);
    ls16(ls, ctx_ls + JC_NUM_IO_BUFFER,    (uint16_t)n_dma);
    ls16(ls, ctx_ls + JC_NUM_CACHE_BUFFER, (uint16_t)n_cache);
    ls32(ls, ctx_ls + JC_O_BUFFER, out_ls);
    ls32(ls, ctx_ls + JC_S_BUFFER, s_ls);
    ls32(ls, ctx_ls + JC_DMA_TAG,  JOB_DMA_TAG);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR,     0);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR + 4, job_ea);

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr,
              "[spurs-job] job 0x%08X: bin 0x%08X+%u -> LS 0 | io=0x%05X(%u, %u dma, %u used)"
              " out=0x%05X(%u) scratch=0x%05X(%u) stack_top=0x%05X(%u) cache=%u"
              " ctx=0x%05X desc=0x%05X(%u)\n",
              job_ea, ea_bin, size_bin, io_ls, size_io, n_dma, io_used,
              out_ls, size_out, s_ls, size_scr, stack_top, size_stk, n_cache,
              ctx_ls, desc_ls, dsz); }

    /* SPURS_JOB_DESCDUMP=1: hexdump the guest job descriptor. The SPU derives
     * its DMA and atomic EAs from these words, so when a job spins on a
     * lock-line address that is not backed, this is what to read first. */
    { static int _d = 0;
      if (options->desc_dump && PS3_LOG_FIRST(_d, 4)) {
          fprintf(stderr, "[spurs-job] desc @0x%08X (%u bytes):", job_ea, dsz);
          for (uint32_t o = 0; o < dsz && o < 128; o += 4) {
              if ((o & 31) == 0) fprintf(stderr, "\n    +%02X:", o);
              fprintf(stderr, " %08X", g32(job_ea + o));
          }
          fprintf(stderr, "\n");
          /* SPURS_JOB_DESCDUMP=2: follow plausible guest EAs in the job user
           * data (past JH_SIZE) and dump what they point at. The job reads its
           * parameters from there, so a wild DMA/atomic address usually
           * originates in one of these blocks. */
          if (options->desc_detail >= 2) {
              for (uint32_t o = JH_SIZE; o < dsz && o < 128; o += 4) {
                  uint32_t v = g32(job_ea + o);
                  if (v < 0x10000u || v >= 0xD0000000u) continue;
                  fprintf(stderr, "[spurs-job]   user+0x%02X -> 0x%08X:", o, v);
                  for (uint32_t q = 0; q < 8; q++)
                      fprintf(stderr, " %08X", g32(v + q * 4));
                  fprintf(stderr, "\n");
              }
          }
      } }

    /* LBP job protocol probe (SPURS_JOB_DUMP): the game's one shared job binary
     * ("JOBCRT Ver13" crt + main @0x1570) reads a be u64 EA out of the
     * descriptor's USER DATA at +0x30, GETs a 128-byte command block from it,
     * then jump-tables on the command TYPE (word1 of the block, cases 0..5).
     * The header's input-DMA-list/io fields are legitimately zero for it. Dump
     * the user data + that command block so the bail path is attributable. */
    uint32_t dump_cmd = 0;
    if (options->dump) {
        uint64_t ud  = g64(job_ea + JH_SIZE);        /* desc+0x30 */
        uint32_t cmd = (uint32_t)ud;                 /* low word = EA */
        fprintf(stderr, "[spurs-job] job 0x%08X userdata: %016llX %016llX %016llX %016llX\n",
                job_ea, (unsigned long long)ud,
                (unsigned long long)g64(job_ea + JH_SIZE + 8),
                (unsigned long long)g64(job_ea + JH_SIZE + 16),
                (unsigned long long)g64(job_ea + JH_SIZE + 24));
        if (cmd && cmd < 0x50000000u) {
            dump_cmd = cmd;
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X:", cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(cmd + o));
            }
            fprintf(stderr, "\n");
            /* cmdblock+0x1C points at a 2048-byte WORK-ITEM table the type-0/1
             * handler GETs (pc=0x1A78) before entering its per-item worker with
             * count = cmdblock+0x24. Dump its head: all-zero means the game
             * never filled the items (upstream/ordering); real entries mean the
             * SPU worker mishandles them. */
            uint32_t tbl = g32(cmd + 0x1C);
            if (tbl && tbl < 0x50000000u) {
                fprintf(stderr, "[spurs-job]   worktable @0x%08X (n=%u):", tbl, g32(cmd + 0x24));
                for (int o = 0; o < 96; o += 4) {
                    if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                    fprintf(stderr, " %08X", g32(tbl + o));
                }
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }

    /* ---- enter the job -------------------------------------------------- */
    /* SPU stack grows down. A function saves its link register in its
     * caller's frame, at 16($sp), so the entry's first store lands 16 bytes
     * above $sp. The cache buffers start right at stack_top: with $sp at
     * stack_top - 16 that save overwrote the first quadword of
     * cacheBuffer[0] (Twisted Metal's Edge jobs read their viewport there and
     * got width = height = 0). Leave the caller's 32-byte frame inside the
     * stack. */
    ctx->gpr[1]._u32[0] = (stack_top - 32u) & ~15u;
    /* SPU ABI R1.word1 is AVAILABLE STACK SPACE, not unused padding.
     * GCC prologues subtract the frame size from every word and use
     * xswd/heqi to detect a negative word1. Leaving it zero made a valid
     * Quiz World job halt on its first checked stack frame (PC 0xFA8). */
    static unsigned abi_stage_active;
    if (options->stage_present && image_id >= options->stage_image)
        __atomic_store_n(&abi_stage_active,1,__ATOMIC_RELEASE);
    if (options->stack ||
        __atomic_load_n(&abi_stage_active,__ATOMIC_ACQUIRE) ||
        (options->min_present && image_id >= options->min_image))
        ctx->gpr[1]._u32[1] = size_stk > 32u ? size_stk - 32u : 0u;
    /* Link register: where the job returns when it is done. The job manager
     * would pass an address inside itself; 0 makes the job re-enter its own
     * entry at LS 0 and run a bogus second lap. */
    ctx->gpr[0]._u32[0] = 0x3FF00u;                   /* SPU_JOB_RETURN_LS */
    ctx->gpr[3]._u32[0] = ctx_ls;                     /* CellSpursJobContext2* */
    ctx->gpr[4]._u32[0] = desc_ls;                    /* CellSpursJob256*      */

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr, "[spurs-job] ENTER r3=%08X r4=%08X r1=%08X entry-fn image=%d\n",
                  ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], ctx->gpr[1]._u32[0],
                  image_id); }
    /* NOTE: after its work the job's runtime jumps to LS 0 ("return to the
     * jm2 kernel" -- real layout has the kernel at 0, jobs above; we load the
     * job at 0). That second lap re-enters the job's crt with dead registers
     * and trips its parameter guard, which HALTS -- ending the run. So the
     * HALT-ASSERT heqi 0x1650 seen once per jm2 job is NORMAL COMPLETION
     * noise, not a failure: the job's real work finished before the jump. */
    static unsigned trace_count[64];
    const int trace_job = options->trace && image_id >= 0 && image_id < 64
                          && __atomic_fetch_add(&trace_count[image_id],1,__ATOMIC_RELAXED) < 8;
    if (trace_job) fprintf(stderr,"[jobtrace] enter image=%d job=%08X sp=%05X available=%u\n",
                          image_id,job_ea,ctx->gpr[1]._u32[0],ctx->gpr[1]._u32[1]);
    const double phase_enter=phase_seconds?job_clock():0;
    if (g_spurs_job_capture_begin) g_spurs_job_capture_begin(ctx, image_id, job_ea);
    spurs_job_enter(entry, image_id, ctx);
    if (g_spurs_job_capture_end) g_spurs_job_capture_end(ctx, image_id, job_ea);
    const double phase_leave=phase_seconds?job_clock():0;
    if (trace_job) fprintf(stderr,"[jobtrace] exit image=%d job=%08X pc=%05X status=%X\n",
                          image_id,job_ea,ctx->pc,ctx->status);
    if (trace_job && ctx->stop_code == 0x3FFF) {
        fprintf(stderr,"[jobtrace] STOP3FFF r3=%08X r4=%08X arg64=%08X/%08X line:",
                ctx->gpr[3]._u32[0],ctx->gpr[4]._u32[0],ctx->gpr[2]._u32[0],ctx->gpr[6]._u32[0]);
        for (unsigned i=0;i<128;i+=16) {
            const uint8_t* q=ctx->ls+0x80+i;
            fprintf(stderr," +%02X=%02X%02X%02X%02X",i,q[0],q[1],q[2],q[3]);
        }
        fprintf(stderr,"\n");
    }

    /* Capture the job's outbound mailbox. These are LS POINTERS TO STRINGS for
     * the SPU printf service, NOT query results -- value 0x83C0 from the sound
     * job points at " compressor, 1/n..." inside its own image. An earlier
     * comment here claimed they carried the title's bus counts; they do not,
     * and feeding them into the completion event turned 0x40000000 into a 1 GB
     * allocation request. Kept because the printf service needs them. */
    if (!exchange) spurs_job_ls_publish(job_owner, ctx->ls);
    /* TEMP SPURS_JOB_LSWORD=<img>:<lsa>: after a job of that image, print the
     * LS word at lsa and every other LS offset holding the same value. */
    { static int img = -2; static uint32_t lsa; static int n = 0;
      if (img == -2) { const char* e = getenv("SPURS_JOB_LSWORD"); img = -1;
          if (e) { img = atoi(e); const char* c = strchr(e, ':'); lsa = c ? (uint32_t)strtoul(c + 1, 0, 16) : 0; } }
      if (img == image_id && n++ < 3) {
          const uint8_t* q = ctx->ls + (lsa & 0x3FFFC);
          uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
          fprintf(stderr, "[lsword] img=%d job=0x%08X LS[0x%05X]=0x%08X io=0x%05X also at:", image_id, job_ea, lsa, v, io_ls);
          for (uint32_t o = 0; o + 4 <= SPU_LS_SIZE; o += 4) {
              const uint8_t* p = ctx->ls + o;
              if (o != (lsa & 0x3FFFC) && ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) == v)
                  fprintf(stderr, " 0x%05X", o);
          }
          fprintf(stderr, "\n");
      } }

    g_spurs_job_mbox      = spu_channel_has_data(&ctx->ch_out_mbox)
                          ? ctx->ch_out_mbox.value : 0;
    g_spurs_job_mbox_intr = spu_channel_has_data(&ctx->ch_out_intr_mbox)
                          ? ctx->ch_out_intr_mbox.value : 0;
    g_spurs_job_mbox_valid = g_spurs_job_mbox || g_spurs_job_mbox_intr;
    { uint32_t _cb = g32(job_ea + 0x4C);
      g_spurs_job_cmd = _cb ? (g32(_cb) >> 16) : 0; }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPURS_JOB_MBOX") ? 1 : 0;
      static int n = 0;
      if (s_t && PS3_LOG_FIRST(n, 16) && g_spurs_job_mbox_valid)
          fprintf(stderr, "[spurs-job] job 0x%08X posted mbox=0x%08X intr=0x%08X\n",
                  job_ea, g_spurs_job_mbox, g_spurs_job_mbox_intr); }

    if (options->dump) {
        fprintf(stderr, "[spurs-job] job 0x%08X exit: status=0x%X stop=0x%X pc=0x%05X\n",
                job_ea, ctx->status, ctx->stop_code, ctx->pc);
        /* Post-run view of the same command block: the game polls result/status
         * fields the job writes back -- a before/after diff shows whether the
         * handler delivered its completion or the PPU waits on stale state. */
        if (dump_cmd) {
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X AFTER:", dump_cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(dump_cmd + o));
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }

    const unsigned phase_mfc=phase_seconds && ctx->mfc_private!=NULL;
    const unsigned phase_atomic=phase_seconds && ctx->dbg_getllar!=0;
    spu_mfc_release(ctx);
    spu_coh_unregister(ctx);   /* no outstanding reservation may retain ctx */
    /* The snapshot's only reader is the printf service, which follows a
     * mailbox the job posted. SPURS_JOB_LS_PUBLISH=mbox hands over only such
     * jobs' LS; others keep their context on this thread, warm in its cache,
     * and the snapshot keeps the last job that did post, the one it names. */
    static SPURS_JOB_TLS int publish_mbox_only = -1;
    if (publish_mbox_only < 0) {
        const char* option = getenv("SPURS_JOB_LS_PUBLISH");
        publish_mbox_only = option && !strcmp(option, "mbox");
    }
    if (exchange && publish_mbox_only && !g_spurs_job_mbox_valid) {
        spurs_job_release_thread_context();
        s_spare_context = ctx;
    } else if (exchange) {
        void* previous = NULL;
        int adopted = spurs_job_ls_exchange(job_owner, ctx, ctx->ls, &previous);
        /* A nested dispatch could have left its own spare. Never leak it or
         * reuse a still-running outer context through the TLS cache. */
        spurs_job_release_thread_context();
        s_spare_context = adopted ? (spu_context*)previous : ctx;
    }
    if (trace_start) {
        uint64_t end = ps3_ft_now();
        if (job_trace_all || end - trace_start >= ps3_ft_min_ns())
            ps3_ft_span(PS3_FT_SPU_JOB, GetCurrentThreadId(), (uint32_t)image_id, job_ea, trace_start, end);
    }
    if(phase_seconds)job_phase_report(image_id,phase_start,phase_enter,phase_leave,job_clock(),phase_seconds,phase_mfc,phase_atomic);
    return 0;
}

/* Keep the original stack-backed path as the control. The exchange path uses
 * the SAME complete context layout, including ls == ls_store: treating an
 * external LS pointer as raw-SPU memory would change mailbox semantics. */
static int run_spurs_job_stack(spu_lifted_entry_fn entry, int image_id,
                               uint32_t job_ea, uint32_t descriptor_size)
{
    spu_context ctx;
    return run_spurs_job_context(entry, image_id, job_ea, descriptor_size, &ctx, 0);
}

int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t descriptor_size)
{
    static SPURS_JOB_TLS int exchange = -1;
    if (exchange < 0) {
        const char* option = getenv("SPURS_JOB_LS_EXCHANGE");
        exchange = option && option[0] && option[0] != '0';
    }
    if (!exchange) return run_spurs_job_stack(entry, image_id, job_ea, descriptor_size);
    if (!s_spare_context) {
#ifdef _WIN32
        s_spare_context = (spu_context*)_aligned_malloc(sizeof(spu_context), 16);
#else
        s_spare_context = (spu_context*)aligned_alloc(16, sizeof(spu_context));
#endif
        if (!s_spare_context)
            return run_spurs_job_stack(entry, image_id, job_ea, descriptor_size);
    }
    spu_context* owned = s_spare_context;
    s_spare_context = NULL; /* a recursively entered job must allocate its own */
    int result = run_spurs_job_context(entry, image_id, job_ea, descriptor_size, owned, 1);
    if (result < 0) { /* rejected before initialization/publication */
        spurs_job_release_thread_context();
        s_spare_context = owned;
    }
    return result;
}

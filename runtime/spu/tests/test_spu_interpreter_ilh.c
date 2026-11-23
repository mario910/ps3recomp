/* ILH must replicate the immediate in every 16-bit lane. Real interpreter
 * bytes plus literal full-vector expectations; no title assets are included. */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CPU-only instructions never reach these external channel/diagnostic hooks. */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
static unsigned lifted_entry;
static void unexpected_entry(spu_context* c) { (void)c; abort(); }
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) {
    (void)c;
    return a && a == lifted_entry ? unexpected_entry : NULL;
}
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}

static void be32(uint8_t* p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
static spu_context ctx;
static int run(const char* name, uint32_t opcode, uint32_t want, u128 lifted) {
    memset(&ctx, 0, sizeof(ctx));
    spu_context_init(&ctx, 0);
    ctx.image_id = -1; /* actual pure interpretation, no registry shortcut */
    memset(&ctx.gpr[80], 0xA5, sizeof(u128));
    memset(&ctx.gpr[79], 0x5A, sizeof(u128));
    be32(ctx.ls + 0x100, opcode);
    be32(ctx.ls + 0x104, 0); /* stop 0 */
    spu_interp_run(&ctx, 0x100);
    assert(ctx.status == SPU_STATUS_STOPPED_BY_STOP && ctx.pc == 0x108);
    const u128 expected = { ._u32 = {want, want, want, want} };
    /* Check all 16 bytes against literal independently derived expectations,
     * then against the existing helper used by lifted instruction emission. */
    int ok = !memcmp(&ctx.gpr[80], &expected, 16) && !memcmp(&ctx.gpr[80], &lifted, 16);
    for (unsigned i = 0; i < 16; ++i) assert(ctx.gpr[79]._u8[i] == 0x5A);
    printf("%s opcode=%08X got=%08X:%08X:%08X:%08X expected=%08X %s\n", name, opcode,
           ctx.gpr[80]._u32[0], ctx.gpr[80]._u32[1], ctx.gpr[80]._u32[2],
           ctx.gpr[80]._u32[3], want, ok ? "PASS" : "FAIL");
    return !ok;
}
/* Compare the streamed reduction against ordinary instruction dispatch on an
 * image with the same bytes. Include the interpreter hand-off boundaries. */
static void test_sample_reduction(void) {
    static const uint32_t words[] = {
        0x38820315, 0x18020316, 0x3fe00f10, 0x1c010306, 0x7802430f,
        0x3b858a94, 0x5822ca13, 0x58440991, 0x5884c387, 0x83c4c811, 0x207ffb0f
    };
    static spu_context reference;
    extern uint64_t g_spu_interp_steps;
    const unsigned pc = 0x2f834;
    uint32_t seed = 0xe0ff91a3u;
    spu_context_init(&ctx, 0);
    spu_context_init(&reference, 0);
    ctx.image_id = 2;
    reference.image_id = 3;
    for (unsigned trial = 0; trial < 256; ++trial) {
        for (unsigned reg = 0; reg < 128; ++reg)
            for (unsigned lane = 0; lane < 4; ++lane) {
                seed = seed * 1664525u + 1013904223u;
                ctx.gpr[reg]._u32[lane] = seed;
            }
        for (unsigned i = 0; i < 512; ++i) {
            ctx.ls[i] = ctx.gpr[i / 4]._u32[i % 4] >> 24;
            ctx.ls[SPU_LS_SIZE - 512 + i] = ctx.gpr[i / 4]._u32[i % 4];
        }
        for (unsigned i = 0; i < sizeof(words)/sizeof(words[0]); ++i)
            be32(ctx.ls + pc + 4*i, words[i]);
        ctx.gpr[6]._u32[0] = 0xfffffff0u + trial % 13;
        ctx.gpr[8]._u32[0] = trial % 9;
        ctx.gpr[9]._u32[0] = ctx.gpr[6]._u32[0] + 4u * (1 + trial % 80);
        unsigned stop = pc + sizeof(words);
        lifted_entry = 0;
        if (trial % 4 == 1) ctx.ls[pc+27] ^= 1; /* different ANDC destination */
        if (trial % 4 == 2) stop = pc + 4*(1 + trial % 10);
        if (trial % 4 == 3) lifted_entry = pc + 4*(1 + trial % 10);
        memcpy(reference.gpr, ctx.gpr, sizeof(ctx.gpr));
        memcpy(reference.ls, ctx.ls, SPU_LS_SIZE);
        uint32_t next = spu_interp_run_until(&ctx, pc, stop);
        uint64_t steps = g_spu_interp_steps;
        assert(next == spu_interp_run_until(&reference, pc, stop));
        assert(steps == g_spu_interp_steps && ctx.status == reference.status && ctx.pc == reference.pc);
        assert(!memcmp(ctx.gpr, reference.gpr, sizeof(ctx.gpr)));
        assert(!memcmp(ctx.ls, reference.ls, SPU_LS_SIZE));
    }
    lifted_entry = 0;
    puts("PASS streamed reduction: exact registers, local store, PC and instruction count");
}

int main(void) {
    test_sample_reduction();
    static const struct { uint16_t imm; uint32_t ilh, il, ilhu; } cases[] = {
        {0x0000, 0x00000000, 0x00000000, 0x00000000},
        {0x0001, 0x00010001, 0x00000001, 0x00010000},
        {0x7FFF, 0x7FFF7FFF, 0x00007FFF, 0x7FFF0000},
        {0x8000, 0x80008000, 0xFFFF8000, 0x80000000},
        {0xBEEF, 0xBEEFBEEF, 0xFFFFBEEF, 0xBEEF0000},
        {0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF0000},
    };
    /* Exact observed retail instruction: ilh $80,0xBEEF. */
    int failed = run("ILH/native-cookie", 0x41DF77D0u, 0xBEEFBEEFu, spu_ilh(0xBEEF));
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        uint16_t imm = cases[i].imm;
        uint32_t operand = ((uint32_t)imm << 7) | 80u;
        failed += run("ILH", 0x41800000u | operand, cases[i].ilh, spu_ilh(imm));
        failed += run("IL", 0x40800000u | operand, cases[i].il, spu_il((int16_t)imm));
        failed += run("ILHU", 0x41000000u | operand, cases[i].ilhu, spu_ilhu(imm));
    }
    printf("SPU immediate loads: %d failures across 19 checks\n", failed);
    return failed ? 1 : 0;
}

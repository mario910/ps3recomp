/* Direct hops between lifted SPU fragments (opt-in per lifted translation
 * unit: SPU_DIRECT_HOPS; included at the end of spu_helpers.h).
 *
 * The lifter lowers every transfer between fragments -- loop back edges
 * included -- as
 *
 *     { ctx->pc = target; g_spu_trampoline_fn = fragment; return; }
 *
 * so each one returns to a drain loop (SPU_DRAIN / spu_drain_call) that runs
 * the per-step hooks and calls the fragment through a pointer. A table probe
 * of two fragments per iteration pays that twice per element; on the Steam
 * Deck the drain loops alone were ~11% of the serial job chain thread.
 *
 * With this header a `return` that has a fragment pending jumps to it
 * directly (a guaranteed tail call, so the host stack does not grow) whenever
 * the hooks the drain loop would run reduce to counting the step:
 *
 *   - the startup-configured hooks allow it (g_spu_direct_hops: no step or
 *     exit trace, no SPU_TAILRET, steps counted, SPU_DIRECT_HOPS != 0);
 *   - no step observer, lockstep, PM-flow trace or stack-reset entry is armed;
 *   - the target is not LS 0 (job end) and not the indirect-branch resolver
 *     (the drain loop decides whether that is a return);
 *   - no interrupt is deliverable.
 *
 * Anything else returns to the drain loop exactly as before. The recent-PC
 * diagnostic rings are not updated for direct hops.
 *
 * Headers included after this one that define functions returning values
 * must bracket themselves with
 *     #pragma push_macro("return") / #undef return ... #pragma pop_macro("return")
 */
#ifndef SPU_DIRECT_HOPS_H
#define SPU_DIRECT_HOPS_H
#if defined(SPU_DIRECT_HOPS) && defined(__clang__) && !defined(__cplusplus)

extern int g_spu_direct_hops;                 /* spu_drain.c: -1 until the first drain step */
extern void (*g_spu_step_observer)(spu_context*, uint32_t);
extern volatile int g_yz_lockstep_on;
extern volatile unsigned g_spu_stack_reset_n;
extern void* volatile g_pm_flow_ctx;

static inline __attribute__((always_inline))
int spu_hop_direct_ok(const spu_context* ctx, void (*next)(spu_context*))
{
    return next != 0 && next != spu_indirect_branch && g_spu_direct_hops > 0 &&
           !g_spu_step_observer && g_yz_lockstep_on == 0 && !g_spu_stack_reset_n &&
           g_pm_flow_ctx != (void*)ctx && ((uint32_t)ctx->pc & SPU_LS_MASK) != 0 &&
           !(ctx->int_enable && (ctx->event_status & ctx->event_mask));
}

#define return                                                              \
    if (spu_hop_direct_ok(ctx, g_spu_trampoline_fn)) {                      \
        void (*spu_hop_next)(spu_context*) = g_spu_trampoline_fn;           \
        g_spu_trampoline_fn = 0;                                            \
        ctx->steps++;                                                       \
        __attribute__((musttail)) return spu_hop_next(ctx);                 \
    } else return

#endif
#endif /* SPU_DIRECT_HOPS_H */

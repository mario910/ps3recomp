#ifndef PS3_SPU_NATIVE_RTZ_H
#define PS3_SPU_NATIVE_RTZ_H
/* EXPERIMENT: single-precision-only SPU images, with MXCSR RZ + FTZ + DAZ
 * established by their execution owner. Not a general double-precision SPU
 * mode. IEEE extended-exponent/overflow cases use exact integer arithmetic.
 * Include after spu_helpers.h (or after its exact scalar definitions). */
#pragma STDC FENV_ACCESS ON
#include <immintrin.h>
#ifndef __FMA__
#error "Native SPU RTZ requires FMA and a checked CPU/OS execution owner"
#endif
#define RTZ_INLINE static inline __attribute__((always_inline))
RTZ_INLINE __m128i rtz_exp(__m128i v) { return _mm_srli_epi32(_mm_slli_epi32(v, 1), 24); }
RTZ_INLINE __m128i rtz_load(u128 v) { return _mm_loadu_si128((const __m128i*)v._u32); }
RTZ_INLINE u128 rtz_store(__m128i v) { u128 out; _mm_storeu_si128((__m128i*)out._u32, v); return out; }
#if defined(SPU_RTZ_RESULT_GUARD)
/* In the owner's RZ+FTZ+DAZ environment, ordinary in-range arithmetic has
 * exactly the SPU result. Non-IEEE operands propagate a nonfinite IEEE result;
 * IEEE overflow under RZ saturates at MAX_FINITE instead of infinity. Reject
 * BOTH classes, including exact MAX_FINITE (a harmless conservative repair).
 * The original operands still reach the same exact integer lane fallback.
 * Not valid for approximate reciprocal/rsqrt, conversions, or other FP modes. */
RTZ_INLINE __m128i rtz_result_valid(__m128 value)
{
    __m128i magnitude=_mm_and_si128(_mm_castps_si128(value),_mm_set1_epi32(0x7fffffff));
    return _mm_cmpgt_epi32(_mm_set1_epi32(0x7f7fffff),magnitude);
}
#endif
#if defined(SPU_RTZ_LEGACY_FIX_ABI)
/* Retained only for standalone A/B code-generation and arithmetic tests. */
#define RTZ_FIX_RETURN u128
#define RTZ_FIX_VALUE(out) (out)
#else
/* A Windows u128 struct return uses a hidden output pointer. The cold helper's
 * result then forces the fast-path value through that same stack slot before
 * every architectural register store. A native vector return keeps the join
 * in XMM; operands, validity predicate and exact lane repair are unchanged. */
#define RTZ_FIX_RETURN __m128i
#define RTZ_FIX_VALUE(out) rtz_load(out)
#endif
static __attribute__((noinline, cold)) RTZ_FIX_RETURN rtz_fix(u128 out, int valid, u128 a, u128 b, u128 c, int op)
{
    for (int n = 0; n < 4; ++n) if (!(valid & (1 << n)))
        out._u32[n] = op == 0 ? spu__fa_lane(a._u32[n], b._u32[n]) :
                      op == 1 ? spu__fm_lane(a._u32[n], b._u32[n]) :
                                spu__fma_lane(a._u32[n], b._u32[n], c._u32[n], 0, 0);
    return RTZ_FIX_VALUE(out);
}
RTZ_INLINE u128 rtz_finish(__m128 value, __m128i valid, u128 a, u128 b, u128 c, int op)
{
    __m128i bits = _mm_castps_si128(value);
    bits = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_slli_epi32(bits, 1), _mm_setzero_si128()), bits);
    int mask = _mm_movemask_ps(_mm_castsi128_ps(valid));
#if defined(SPU_RTZ_LEGACY_FIX_ABI)
    u128 out = rtz_store(bits);
    return mask == 15 ? out : rtz_fix(out, mask, a, b, c, op);
#else
    if (mask != 15) bits = rtz_fix(rtz_store(bits), mask, a, b, c, op);
    return rtz_store(bits);
#endif
}
RTZ_INLINE u128 spu_native_rtz_fa(u128 a, u128 b, uint32_t negb)
{
    __m128i x = rtz_load(a), y = _mm_xor_si128(rtz_load(b), _mm_set1_epi32((int)negb));
    __m128 value=_mm_add_ps(_mm_castsi128_ps(x),_mm_castsi128_ps(y));
#if defined(SPU_RTZ_RESULT_GUARD)
    __m128i valid=rtz_result_valid(value);
#else
    __m128i valid = _mm_and_si128(_mm_cmpgt_epi32(_mm_set1_epi32(254), rtz_exp(x)),
                                  _mm_cmpgt_epi32(_mm_set1_epi32(254), rtz_exp(y)));
#endif
    return rtz_finish(value,valid,a,rtz_store(y),a,0);
}
RTZ_INLINE u128 spu_native_rtz_fm(u128 a, u128 b)
{
    __m128i x = rtz_load(a), y = rtz_load(b);
    __m128 value=_mm_mul_ps(_mm_castsi128_ps(x),_mm_castsi128_ps(y));
#if defined(SPU_RTZ_RESULT_GUARD)
    __m128i valid=rtz_result_valid(value);
#else
    __m128i ex = rtz_exp(x), ey = rtz_exp(y);
    __m128i valid = _mm_and_si128(_mm_cmpgt_epi32(_mm_set1_epi32(255), ex),
                                  _mm_cmpgt_epi32(_mm_set1_epi32(255), ey));
    valid = _mm_and_si128(valid, _mm_cmpgt_epi32(_mm_set1_epi32(381), _mm_add_epi32(ex, ey)));
#endif
    return rtz_finish(value, valid, a, b, a, 1);
}
RTZ_INLINE u128 spu_native_rtz_fma(u128 a, u128 b, u128 c, uint32_t negp, uint32_t negc)
{
    __m128i x = _mm_xor_si128(rtz_load(a), _mm_set1_epi32((int)negp));
    __m128i y = rtz_load(b), z = _mm_xor_si128(rtz_load(c), _mm_set1_epi32((int)negc));
    __m128 value=_mm_fmadd_ps(_mm_castsi128_ps(x),_mm_castsi128_ps(y),_mm_castsi128_ps(z));
#if defined(SPU_RTZ_RESULT_GUARD)
    __m128i valid=rtz_result_valid(value);
#else
    __m128i ex = rtz_exp(x), ey = rtz_exp(y);
    __m128i valid = _mm_and_si128(_mm_cmpgt_epi32(_mm_set1_epi32(255), ex),
                                  _mm_cmpgt_epi32(_mm_set1_epi32(255), ey));
    valid = _mm_and_si128(valid, _mm_cmpgt_epi32(_mm_set1_epi32(380), _mm_add_epi32(ex, ey)));
    valid = _mm_and_si128(valid, _mm_cmpgt_epi32(_mm_set1_epi32(254), rtz_exp(z)));
#endif
    return rtz_finish(value,
                      valid, rtz_store(x), b, rtz_store(z), 2);
}
#undef RTZ_INLINE
#undef RTZ_FIX_RETURN
#undef RTZ_FIX_VALUE
#endif

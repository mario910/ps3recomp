#pragma once
/* Same image-scoped RZ/FTZ/DAZ contract as spu_native_rtz.h. Only ordinary
 * exponent ranges use hardware conversion. Preserve the exact SPU extended
 * exponent and saturation semantics through the integer reference otherwise. */
#define RTZ_CVT_INLINE static inline __attribute__((always_inline))
RTZ_CVT_INLINE __m128 rtz_pow2(int exponent)
{
    return _mm_castsi128_ps(_mm_set1_epi32((exponent+127)<<23));
}
static __attribute__((noinline,cold)) u128 rtz_cfl_fix(u128 out,int valid,u128 a,int i8,int unsign)
{
    for (int n=0;n<4;++n) if (!(valid&(1<<n)))
        out._u32[n]=spu__cfl_lane(a._u32[n],173-(i8&255),unsign);
    return out;
}
RTZ_CVT_INLINE u128 spu_csflt(u128 a,int i8)
{
    const int scale=(i8&255)-155;
    if (scale < -126 || scale > 96) return spu_csflt_ref(a,i8);
    return rtz_store(_mm_castps_si128(_mm_mul_ps(_mm_cvtepi32_ps(rtz_load(a)),rtz_pow2(scale))));
}
RTZ_CVT_INLINE u128 spu_cuflt(u128 a,int i8)
{
    const int scale=(i8&255)-155;
    if (scale < -126 || scale > 96) return spu_cuflt_ref(a,i8);
    __m128i bits=rtz_load(a);
    __m128 lo=_mm_cvtepi32_ps(_mm_and_si128(bits,_mm_set1_epi32(0x7fffffff)));
    __m128 hi=_mm_castsi128_ps(_mm_and_si128(_mm_srai_epi32(bits,31),_mm_set1_epi32(0x4f000000)));
    /* Both terms are nonnegative. Truncating the low term before the add
     * cannot cross a larger final significand boundary under RZ. */
    return rtz_store(_mm_castps_si128(_mm_mul_ps(_mm_add_ps(lo,hi),rtz_pow2(scale))));
}
RTZ_CVT_INLINE u128 rtz_cfl(u128 a,int i8,int unsign)
{
    const int scale=173-(i8&255);
    if (scale > 127) return unsign ? spu_cfltu_ref(a,i8) : spu_cflts_ref(a,i8);
    __m128i bits=rtz_load(a);
    __m128i good=_mm_cmpgt_epi32(_mm_set1_epi32(255),rtz_exp(bits));
    __m128 value=_mm_mul_ps(_mm_castsi128_ps(bits),rtz_pow2(scale));
    __m128i result;
    if (unsign) {
        value=_mm_max_ps(value,_mm_setzero_ps());
        __m128 high=_mm_cmpge_ps(value,_mm_set1_ps(2147483648.0f));
        __m128 reduced=_mm_sub_ps(value,_mm_and_ps(high,_mm_set1_ps(2147483648.0f)));
        result=_mm_xor_si128(_mm_cvttps_epi32(reduced),
                            _mm_and_si128(_mm_castps_si128(high),_mm_set1_epi32((int)0x80000000u)));
        result=_mm_or_si128(result,_mm_castps_si128(_mm_cmpge_ps(value,_mm_set1_ps(4294967296.0f))));
    } else {
        result=_mm_cvttps_epi32(value);
        __m128i overflow=_mm_castps_si128(_mm_cmpge_ps(value,_mm_set1_ps(2147483648.0f)));
        result=_mm_or_si128(_mm_andnot_si128(overflow,result),
                            _mm_and_si128(overflow,_mm_set1_epi32(0x7fffffff)));
    }
    int mask=_mm_movemask_ps(_mm_castsi128_ps(good));
    u128 out=rtz_store(result);
    return mask==15 ? out : rtz_cfl_fix(out,mask,a,i8,unsign);
}
RTZ_CVT_INLINE u128 spu_cflts(u128 a,int i8) { return rtz_cfl(a,i8,0); }
RTZ_CVT_INLINE u128 spu_cfltu(u128 a,int i8) { return rtz_cfl(a,i8,1); }
#undef RTZ_CVT_INLINE

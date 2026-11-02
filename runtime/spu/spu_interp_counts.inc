/* Included by spu_interp.c so existing standalone interpreter fixtures keep
 * linking without an additional runtime object. Public atomics are map-bound
 * diagnostic data; external readers must suspend every target thread first. */
#include "spu_interp_counts.h"
#include <stdatomic.h>
#include <limits.h>

_Atomic uint64_t g_spu_interp_exact_descriptor=SPU_INTERP_COUNT_DESCRIPTOR;
_Atomic int32_t g_spu_interp_exact_enabled=-1;
_Atomic uint32_t g_spu_interp_exact_flags;
_Atomic uint64_t g_spu_interp_exact_unbucketed;
_Atomic uint64_t g_spu_interp_exact_keys[SPU_INTERP_COUNT_CAPACITY];
_Atomic uint64_t g_spu_interp_exact_steps[SPU_INTERP_COUNT_CAPACITY];

uint64_t spu_interp_counts_descriptor(void)
{
    return atomic_load_explicit(&g_spu_interp_exact_descriptor,memory_order_relaxed);
}
int spu_interp_counts_enabled(void)
{
    if(spu_interp_counts_descriptor()!=SPU_INTERP_COUNT_DESCRIPTOR)abort();
    int32_t mode=atomic_load_explicit(&g_spu_interp_exact_enabled,memory_order_acquire);
    if(mode<0){
        const char* text=getenv("SPU_INTERP_EXACT");
        if(text && ((text[0]!='0' && text[0]!='1') || text[1])){
            fprintf(stderr,"[spu-interp-exact] invalid SPU_INTERP_EXACT;need0/1\n");abort();
        }
        int32_t wanted=text && text[0]=='1',expected=-1;
        if(atomic_compare_exchange_strong_explicit(&g_spu_interp_exact_enabled,&expected,wanted,
                memory_order_acq_rel,memory_order_acquire))mode=wanted;
        else mode=expected;
    }
    return mode==1;
}
static void spu_interp_count_increment(_Atomic uint64_t* value)
{
    uint64_t previous=atomic_load_explicit(value,memory_order_relaxed);
    for(;;){
        if(previous==UINT64_MAX){atomic_fetch_or_explicit(&g_spu_interp_exact_flags,SPU_INTERP_COUNT_OVERFLOW,memory_order_relaxed);return;}
        if(atomic_compare_exchange_weak_explicit(value,&previous,previous+1,memory_order_relaxed,memory_order_relaxed))return;
    }
}
static void spu_interp_count_instruction(const spu_context* ctx)
{
    static SPU_THREAD_LOCAL uint64_t last_key;
    static SPU_THREAD_LOCAL unsigned last_slot;
    const uint64_t key=(((uint64_t)(uint32_t)ctx->image_id<<8)|((ctx->pc&SPU_LS_MASK)>>10))+1;
    if(atomic_load_explicit(&g_spu_interp_exact_flags,memory_order_relaxed)&SPU_INTERP_COUNT_FULL){
        spu_interp_count_increment(&g_spu_interp_exact_unbucketed);return;
    }
    if(key!=last_key){
        uint64_t mixed=key;unsigned probe;
        mixed^=mixed>>30;mixed*=UINT64_C(0xbf58476d1ce4e5b9);
        mixed^=mixed>>27;mixed*=UINT64_C(0x94d049bb133111eb);mixed^=mixed>>31;
        for(probe=0;probe<SPU_INTERP_COUNT_CAPACITY;++probe){
            const unsigned slot=((unsigned)mixed+probe)&(SPU_INTERP_COUNT_CAPACITY-1);
            uint64_t found=atomic_load_explicit(&g_spu_interp_exact_keys[slot],memory_order_acquire);
            if(!found){
                uint64_t expected=0;
                if(atomic_compare_exchange_strong_explicit(&g_spu_interp_exact_keys[slot],&expected,key,
                        memory_order_acq_rel,memory_order_acquire))found=key;
                else found=expected;
            }
            if(found==key){last_key=key;last_slot=slot;break;}
        }
        if(probe==SPU_INTERP_COUNT_CAPACITY){
            atomic_fetch_or_explicit(&g_spu_interp_exact_flags,SPU_INTERP_COUNT_FULL,memory_order_relaxed);
            spu_interp_count_increment(&g_spu_interp_exact_unbucketed);return;
        }
    }
    spu_interp_count_increment(&g_spu_interp_exact_steps[last_slot]);
}
size_t spu_interp_counts_read(spu_interp_count_row* rows,size_t capacity,spu_interp_count_info* info)
{
    spu_interp_count_info state;
    state.enabled=atomic_load_explicit(&g_spu_interp_exact_enabled,memory_order_acquire);
    state.flags=atomic_load_explicit(&g_spu_interp_exact_flags,memory_order_relaxed);
    state.unbucketed_steps=atomic_load_explicit(&g_spu_interp_exact_unbucketed,memory_order_relaxed);
    state.total=state.unbucketed_steps;
    size_t count=0;
    for(unsigned slot=0;slot<SPU_INTERP_COUNT_CAPACITY;++slot){
        const uint64_t key=atomic_load_explicit(&g_spu_interp_exact_keys[slot],memory_order_acquire);
        const uint64_t steps=atomic_load_explicit(&g_spu_interp_exact_steps[slot],memory_order_relaxed);
        if(!steps)continue;
        if(!key){state.flags|=SPU_INTERP_COUNT_OVERFLOW;continue;}
        const uint64_t raw=key-1;
        if(rows && count<capacity){rows[count].image=(int32_t)(uint32_t)(raw>>8);rows[count].page=(uint32_t)(raw&255)<<10;rows[count].steps=steps;}
        ++count;
        if(UINT64_MAX-state.total<steps){state.flags|=SPU_INTERP_COUNT_TOTAL_OVERFLOW;state.total=UINT64_MAX;}
        else state.total+=steps;
    }
    if(info)*info=state;
    return count;
}

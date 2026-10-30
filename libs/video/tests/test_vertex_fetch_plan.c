/*
 * rsx_vertex_fetch_plan_prepare against the per-attribute walk it replaced.
 *
 * _prepare finds the span of ordinary per-vertex attributes (frequency 0/1)
 * once per draw instead of once per attribute. Random fetch plans -- every
 * vertex type, strides and offsets, frequency dividers in both modes, guest
 * memory that is partly unmapped, INLINE_ARRAY streams -- with random index
 * references, some past the 20-bit element wrap: every attribute's stable
 * range (base, first, last) must be what the old walk gave.
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *     -o /tmp/test_vertex_fetch_plan libs/video/tests/test_vertex_fetch_plan.c \
 *     libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c
 */
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3_ftrace.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 20) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

enum { GUEST = 1 << 16 };
static uint8_t guest[2][GUEST];         /* two locations */
static uint32_t mapped_end[2];          /* bytes past this are unmapped */
static u32 resolve_calls;

static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    resolve_calls++;
    const u32 loc = location & 1u;
    if ((u64)offset + min_bytes > mapped_end[loc]) return NULL;
    return guest[loc] + offset;
}

/* The walk _prepare used to do: every reference, once per attribute. */
static void ref_prepare(rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count || !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;
        if (plan->inline_data) {
            const u32 stride = plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }
        if (!plan->guest_ptr)
            continue;
        u32 first = UINT_MAX, last = 0;
        for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index, fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }
        const u64 prefix = (u64)plan->base_offset + (u64)fetch->desc.offset;
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end = prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start || end - start > UINT_MAX)
            continue;
        const u8* base = plan->guest_ptr(plan->guest_user, fetch->desc.location,
                                         (u32)start, (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

static void random_plan(rsx_vertex_fetch_plan* plan, int use_inline,
                        const u8* inl, u32 inl_bytes)
{
    memset(plan, 0, sizeof *plan);
    rsx_vertex_layout_plan_init(&plan->layout, rnd() & 0xFFFFu);
    plan->base_offset = (rnd() % 4 == 0) ? rnd() % 256u : 0;
    plan->divider_mask = rnd() & 0xFFFFu;
    plan->guest_ptr = guest_ptr;
    u32 inline_packed = 0;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* f = &plan->attr[a];
        const u32 r = rnd() % 16;
        f->desc.type = r < 6 ? RSX_VTX_TYPE_FLOAT : r < 7 ? 0 : 1 + rnd() % 7;
        f->desc.size = (rnd() % 8 == 0) ? rnd() % 6 : 1 + rnd() % 4;
        f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
        f->desc.stride = (rnd() % 4 == 0) ? 0 : f->elem_size + (rnd() % 3) * 4u;
        f->stride = f->desc.stride ? f->desc.stride : f->elem_size;
        f->desc.frequency = (rnd() % 4 == 0) ? rnd() % 4 : 0;
        f->desc.offset = rnd() % 4096u;
        f->desc.location = rnd() & 1u;
    }
    if (use_inline) {
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
            plan->inline_off[a] = inline_packed;
            if (plan->attr[a].desc.type && plan->attr[a].desc.size)
                inline_packed += plan->attr[a].elem_size;
        }
        plan->inline_stride = inline_packed ? inline_packed : 16u;
        plan->inline_data = inl;
        plan->inline_bytes = inl_bytes;
    }
}

static void compare_fetch(const rsx_vertex_fetch_plan* plan,const rsx_vertex_ref* refs,u32 count)
{
    static _Alignas(16) u8 legacy[200*256+16],bulk[200*256+16];
    const size_t bytes=(size_t)count*plan->layout.stride;
    CHECK(count<=200 && bytes+16<=sizeof legacy,"fixture output capacity");
    if(count>200 || bytes+16>sizeof legacy)return;
    memset(legacy,0xa5,sizeof legacy);memset(bulk,0xa5,sizeof bulk);
    int ref_ok=1;resolve_calls=0;
    for(u32 i=0;i<count;i++)
        if(!rsx_vertex_fetch_one(plan,&refs[i],plan->layout.stride?legacy+(size_t)i*plan->layout.stride:NULL)){
            ref_ok=0;break;
        }
    const u32 ref_calls=resolve_calls;resolve_calls=0;
    const int bulk_ok=rsx_vertex_fetch_many(plan,refs,count,bulk);
    CHECK((bulk_ok!=0)==ref_ok,"bulk/reference success mismatch");
    CHECK(resolve_calls==ref_calls,"bulk/reference fallback resolver calls %u/%u",resolve_calls,ref_calls);
    if(ref_ok && bulk_ok)CHECK(!memcmp(legacy,bulk,bytes),"bulk/reference output differs");
    for(size_t i=bytes;i<bytes+16;i++)CHECK(bulk[i]==0xa5,"bulk write crossed output end");
}

static void structured_fetch(void)
{
    rsx_vertex_ref refs[200];
    for(u32 i=0;i<200;i++){refs[i].vertex_id=(i*37)%200;refs[i].base_index=0;}
    for(u32 type=1;type<=7;type++)for(u32 size=1;size<=4;size++)for(u32 mode=0;mode<3;mode++){
        rsx_vertex_fetch_plan plan;memset(&plan,0,sizeof plan);
        rsx_vertex_layout_plan_init(&plan.layout,(1u<<0)|(1u<<3)|(1u<<8));
        plan.guest_ptr=guest_ptr;plan.divider_mask=mode==2?1u<<3:0;
        const u32 attrs[2]={0,3};
        for(u32 k=0;k<2;k++){
            rsx_vertex_fetch_attr* f=&plan.attr[attrs[k]];
            f->desc.type=k?type:RSX_VTX_TYPE_FLOAT;f->desc.size=size;
            f->elem_size=rsx_vertex_attrib_size(f->desc.type,size);
            f->stride=f->elem_size+3;f->desc.offset=k?16384:0;
            f->desc.location=k;f->desc.frequency=k && mode?3:0;
        }
        plan.attr[8].default_value[0]=0.25f;plan.attr[8].default_value[3]=-0.0f;
        mapped_end[0]=mapped_end[1]=GUEST;
        rsx_vertex_fetch_plan_prepare(&plan,refs,200);compare_fetch(&plan,refs,200);
        refs[199].vertex_id=4095;compare_fetch(&plan,refs,200);refs[199].vertex_id=(199*37)%200;
        mapped_end[1]=16384+200;rsx_vertex_fetch_plan_prepare(&plan,refs,200);compare_fetch(&plan,refs,200);
        mapped_end[0]=200;rsx_vertex_fetch_plan_prepare(&plan,refs,200);compare_fetch(&plan,refs,200);
    }
}

static int benchmark(void)
{
    rsx_vertex_fetch_plan plan;memset(&plan,0,sizeof plan);
    rsx_vertex_layout_plan_init(&plan.layout,0xf);
    rsx_vertex_ref refs[1024];static _Alignas(16) u8 out[1024*64];
    for(u32 a=0;a<4;a++){
        rsx_vertex_fetch_attr* f=&plan.attr[a];f->desc.type=RSX_VTX_TYPE_FLOAT;
        f->desc.size=a==0?3:4;f->stride=16;f->stable_base=guest[0]+a*16384;
        f->stable_last=1023;
    }
    for(u32 i=0;i<1024;i++){refs[i].vertex_id=(i*37)%1024;refs[i].base_index=0;}
    volatile u32 checksum=0;
    const u32 counts[]={3,16,64,256,1024};
    for(u32 c=0;c<sizeof counts/sizeof counts[0];c++){
        const u32 count=counts[c],iterations=1000000/count+1;
        u64 start=ps3_ft_now();
        for(u32 n=0;n<iterations;n++){
            for(u32 i=0;i<count;i++)rsx_vertex_fetch_one(&plan,&refs[i],out+i*64);
            checksum+=out[n%(count*64)];
        }
        const u64 old=ps3_ft_now()-start;start=ps3_ft_now();
        for(u32 n=0;n<iterations;n++){
            rsx_vertex_fetch_many(&plan,refs,count,out);checksum+=out[n%(count*64)];
        }
        const u64 bulk=ps3_ft_now()-start;
        printf("vertices=%u legacy_ms=%.3f bulk_ms=%.3f speedup=%.3f\n",count,old/1e6,bulk/1e6,(double)old/bulk);
    }
    printf("benchmark checksum=%u\n",checksum);return 0;
}

int main(int argc,char** argv)
{
    enum { PLANS = 6000, MAXV = 200 };
    static rsx_vertex_ref refs[MAXV];
    static u8 inl[8192];
    int stable = 0, divided = 0;
    for(u32 l=0;l<2;l++)for(u32 i=0;i<GUEST;i++)guest[l][i]=(u8)(i*13+i/17+l*91);
    for(u32 i=0;i<sizeof inl;i++)inl[i]=(u8)(i*31+i/7);
    if(argc==2 && !strcmp(argv[1],"--bench"))return benchmark();
    if(argc!=1){printf("unknown test option\n");return 1;}
    structured_fetch();

    for (int n = 0; n < PLANS; n++) {
        for (int l = 0; l < 2; l++)
            mapped_end[l] = (rnd() % 3 == 0) ? rnd() % GUEST : GUEST;
        rsx_vertex_fetch_plan plan, ref;
        random_plan(&plan, rnd() % 5 == 0, inl, (u32)(rnd() % sizeof inl));

        const u32 count = 1 + rnd() % MAXV;
        const u32 base = (rnd() % 4 == 0) ? rnd() % 64u : 0;
        /* Some runs sit just below the 20-bit element wrap and cross it. */
        const u32 start = (rnd() % 4 == 0) ? 0x000FFFFFu - rnd() % 400u : 0;
        for (u32 i = 0; i < count; i++) {
            refs[i].vertex_id = start + ((rnd() % 8 == 0) ? rnd() : rnd() % 300u);
            refs[i].base_index = base;
        }
        ref = plan;
        rsx_vertex_fetch_plan_prepare(&plan, refs, count);
        compare_fetch(&plan,refs,count);
        ref_prepare(&ref, refs, count);
        for (u32 at = 0; at < RSX_DSP_NUM_VERTEX_ATTR; at++) {
            CHECK(plan.attr[at].stable_base == ref.attr[at].stable_base &&
                  plan.attr[at].stable_first == ref.attr[at].stable_first &&
                  plan.attr[at].stable_last == ref.attr[at].stable_last &&
                  plan.attr[at].stride == ref.attr[at].stride,
                  "plan %d attr %u: span %u..%u, walk %u..%u", n, at,
                  plan.attr[at].stable_first, plan.attr[at].stable_last,
                  ref.attr[at].stable_first, ref.attr[at].stable_last);
            if (ref.attr[at].stable_base) {
                stable++;
                if (plan.attr[at].desc.frequency > 1) divided++;
            }
        }
    }

    printf("test_vertex_fetch_plan: %d plans, %d stable ranges (%d divided), %d failure(s)\n",
           PLANS, stable, divided, fails);
    if (stable < PLANS || divided < 100) {
        printf("FAIL: too few cases exercised a path\n");
        return 1;
    }
    rsx_vertex_fetch_plan empty;memset(&empty,0,sizeof empty);
    CHECK(rsx_vertex_fetch_many(&empty,NULL,0,NULL)==1,"empty bulk fetch");
    CHECK(rsx_vertex_fetch_many(NULL,NULL,0,NULL)==0,"null bulk plan rejected");
    return fails ? 1 : 0;
}

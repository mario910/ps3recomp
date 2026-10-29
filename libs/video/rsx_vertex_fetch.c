/*
 * ps3recomp - RSX guest vertex fetch (see rsx_vertex_fetch.h)
 */
#include "rsx_vertex_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float rsx_rd_bef(const u8* p)
{
    u32 w; memcpy(&w, p, 4);
    w = ((w >> 24) & 0xFFu) | ((w >> 8) & 0xFF00u) |
        ((w << 8) & 0xFF0000u) | ((w << 24) & 0xFF000000u);
    float f; memcpy(&f, &w, 4); return f;
}

float rsx_rd_half_be(const u8* p)
{
    u16 h = (u16)((p[0] << 8) | p[1]);
    u32 sgn = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu, f;
    if (exp == 0)        f = sgn << 31;
    else if (exp == 31)  f = (sgn << 31) | 0x7F800000u | (man << 13);
    else                 f = (sgn << 31) | ((exp - 15u + 127u) << 23) | (man << 13);
    float o; memcpy(&o, &f, 4); return o;
}

void rsx_rd_cmp_be(const u8* p,float out[4])
{
    /* RSX CMP32: signed X/Y in low 11-bit fields, Z in the high 10 bits.
     * Expand into signed 16-bit lanes before dividing by 32767, matching
     * RPCS3 RSXVertexFetch.glsl (not ordinary 10/11-bit endpoint SNORM).
     * Never reinterpret this word as float: valid packed normals can have
     * IEEE NaN bit patterns, blanking whole lit surfaces. */
    u32 word=((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
    int x=(int)(word&0x7FFu),y=(int)((word>>11)&0x7FFu),z=(int)(word>>22);
    if (x&0x400) x-=0x800;
    if (y&0x400) y-=0x800;
    if (z&0x200) z-=0x400;
    out[0]=(float)(x*32)/32767.0f;
    out[1]=(float)(y*32)/32767.0f;
    out[2]=(float)(z*64)/32767.0f;
    out[3]=1.0f;
}

static void rsx_decode_attrib(u32 type,u32 size,const u8* p,float out[4],int idx)
{
    u32 n=size?size:4;if(n>4)n=4;
    switch(type) {
    case 2: for(u32 k=0;k<n;++k)out[k]=rsx_rd_bef(p+k*4);break;
    case 3: for(u32 k=0;k<n;++k)out[k]=rsx_rd_half_be(p+k*2);break;
    case 4: for(u32 k=0;k<n;++k)out[k]=p[k]/255.0f;break;
    case 1: for(u32 k=0;k<n;++k)out[k]=(float)(s16)((p[k*2]<<8)|p[k*2+1])/32767.0f;break;
    case 5: for(u32 k=0;k<n;++k)out[k]=(float)(s16)((p[k*2]<<8)|p[k*2+1]);break;
    case 6: rsx_rd_cmp_be(p,out);break;
    case 7: for(u32 k=0;k<n;++k)out[k]=(float)p[k];break;
    default:
        { static int t=-1;if(t<0)t=getenv("VTX_TYPEDBG")?1:0;
          if(t){static u32 seen=0;if(!(seen&(1u<<(type&31)))){seen|=1u<<(type&31);
              fprintf(stderr,"[VTXTYPE] UNHANDLED type=%u attr=%d size=%u\n",type,idx,size);}} }
        for(u32 k=0;k<n;++k)out[k]=rsx_rd_bef(p+k*4);break;
    }
}

void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{
    /* A disabled attribute array feeds the CONSTANT vertex attribute register
     * (NV4097_SET_VERTEX_DATA4F_M), not zero -- same rule as glColor4f with
     * the colour array off. Defaulting these to black multiplied Rubber
     * Ducky's duck texture away in the fragment program. Components the array
     * does not supply (size < 4) keep the register's value too. */
    out[0] = st->vertex_data4f[idx][0];
    out[1] = st->vertex_data4f[idx][1];
    out[2] = st->vertex_data4f[idx][2];
    out[3] = st->vertex_data4f[idx][3];

    const rsx_vertex_attrib* a = &st->vertex_attribs[idx];
    if (!a->enabled || a->stride == 0 || !vm_base) return;

    /* Vertex frequency divisor (instancing). freq 0/1 = per-vertex. For
     * freq > 1 the element index is either vertex/freq (DIVIDE: per-instance
     * data advances once per freq verts) or vertex%freq (MODULO: a mesh
     * repeats every freq verts). NV4097_SET_FREQUENCY_DIVIDER_OPERATION's
     * per-attribute bit selects MODULO. DeferredShading's cube rings need
     * this: the shared cube mesh is MODULO, the per-instance transform in
     * attrib9 is DIVIDE -- without it attrib9 advanced every vertex and the
     * instanced cubes drew with garbage transforms (only the baseplate,
     * which isn't instanced, survived). */
    u32 ei = vi;
    if (a->frequency > 1) {
        static int nofreq = -1;                 /* VP_NOFREQ: instancing kill-switch */
        if (nofreq < 0) nofreq = getenv("VP_NOFREQ") ? 1 : 0;
        if (!nofreq)
            ei = (st->frequency_divider_op & (1u << idx))
                     ? (vi % a->frequency)      /* MODULO: repeat mesh */
                     : (vi / a->frequency);     /* DIVIDE: per-instance */
    }

    /* Bit 31 of the vertex-array OFFSET selects the context DMA: 0 = LOCAL
     * (VRAM), 1 = MAIN (IO-mapped system memory). Resolve LOCAL as local --
     * routing it through cellGcmResolveOffset lets the IO table win for any
     * offset whose page the IO region also covers. One title's vertex arrays
     * sit at offsets like 0x4480, shadowed by its 1MB IO window at 0x11100000:
     * the array was uploaded to VRAM 0xC0004480 but resolved to main
     * 0x11104480, which is empty -- so every INDEXED mesh fetched zeros and
     * collapsed to the origin, while non-indexed geometry whose arrays lie
     * outside the shadowed pages drew fine. */
    u32 off = (a->offset & 0x7FFFFFFFu) + ei * a->stride;
    const u8* p = vm_base + ((a->offset & 0x80000000u)
        ? cellGcmResolveLocated(0, off)   /* MAIN:  IO offset table          */
        : cellGcmResolveLocated(1, off)); /* LOCAL: VRAM, never the IO table */

    rsx_decode_attrib(a->type,a->size,p,out,idx);
}

void rsx_fetch_vertex(const rsx_state* st, u32 vi, float out[16][4])
{
    memcpy(out, st->vertex_data4f, sizeof(st->vertex_data4f));
    if (!vm_base) return;
    for (int n = 0; n < 16; ++n)
        if (st->vertex_attribs[n].enabled && st->vertex_attribs[n].stride)
            rsx_fetch_attrib(st, n, vi, out[n]);
}

void rsx_vertex_draw_plan_init(rsx_vertex_draw_plan* plan,const rsx_state* state)
{
    static int nofreq=-1;if(nofreq<0)nofreq=getenv("VP_NOFREQ")?1:0;
    plan->state=state;plan->count=0;
    for(unsigned n=0;n<16;++n){const rsx_vertex_attrib* a=&state->vertex_attribs[n];
        if(!a->enabled || !a->stride)continue;
        rsx_vertex_planned_attr* p=&plan->attrs[plan->count++];
        p->idx=(u8)n;p->type=a->type;p->size=a->size;p->local=!(a->offset&0x80000000u);
        p->offset=a->offset&0x7fffffffu;p->stride=a->stride;
        p->frequency=nofreq?0:a->frequency;p->operation=(state->frequency_divider_op>>n)&1u;p->mapped=0;
    }
}
void rsx_fetch_vertex_planned(rsx_vertex_draw_plan* plan,u32 vi,float out[16][4])
{
    memcpy(out,plan->state->vertex_data4f,sizeof(plan->state->vertex_data4f));
    if(!vm_base)return;
    for(unsigned n=0;n<plan->count;++n){rsx_vertex_planned_attr* a=&plan->attrs[n];
        u32 ei=vi;if(a->frequency>1)ei=a->operation?vi%a->frequency:vi/a->frequency;
        u32 off=a->offset+ei*a->stride; /* Preserve the reference's 32-bit wrap. */
        u32 page=off>>20;
        if(!a->mapped || a->page!=page){
            /* Resolve the actual element address, not an assumed aligned EA.
             * cellGcmResolveLocated is affine within each 1-MiB IO page. */
            a->ea_bias=cellGcmResolveLocated(a->local,off)-off;a->page=page;a->mapped=1;
        }
        rsx_decode_attrib(a->type,a->size,vm_base+(u32)(a->ea_bias+off),out[a->idx],a->idx);
    }
}

void rsx_vertex_cache_init(rsx_vertex_cache* cache, const rsx_state* state)
{
    cache->state = state;
    cache->planned=0;
    cache->valid = 0; /* do not clear the 16 KiB payload */
}
void rsx_vertex_cache_init_planned(rsx_vertex_cache* cache,const rsx_state* state)
{
    rsx_vertex_cache_init(cache,state);rsx_vertex_draw_plan_init(&cache->plan,state);cache->planned=1;
}

void rsx_fetch_vertex_cached(rsx_vertex_cache* cache, u32 vi, float out[16][4])
{
    unsigned slot = vi & 63u;
    u64 bit = (u64)1 << slot;
    if (!(cache->valid & bit) || cache->indices[slot] != vi) {
        if(cache->planned)rsx_fetch_vertex_planned(&cache->plan,vi,cache->vertices[slot]);
        else rsx_fetch_vertex(cache->state, vi, cache->vertices[slot]);
        cache->indices[slot] = vi;
        cache->valid |= bit;
    }
    memcpy(out, cache->vertices[slot], sizeof cache->vertices[slot]);
}

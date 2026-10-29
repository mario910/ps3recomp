/* GPU atlas cells must be sampled from their producing surface, not stale
 * CPU memory. Keep a separate resource for each cell: two players of equal
 * dimensions must not share the last cell selected during the SRV pre-pass. */
static int rt_sub_rect(int index,const D3D12DrawRecord* dr,unsigned unit,rsx_rt_rect* rect)
{
    const OffRT* r=&s_d3d.off_rt[index];
    if(r->sx*r->sy!=1 || !(dr->tex[unit].fmt&0x20))return 0;
    const u32 bpp=r->dxgi==DXGI_FORMAT_R16G16B16A16_FLOAT?8u:
                  r->dxgi==DXGI_FORMAT_R32G32B32A32_FLOAT?16u:4u;
    const u32 pitch=r->pitch?r->pitch:r->w*bpp;
    const u32 sample_pitch=dr->tex[unit].pitch?dr->tex[unit].pitch:dr->tex[unit].w*bpp;
    return rsx_rt_region(r->off,r->w,r->h,pitch,bpp,dr->tex[unit].raw,
                         dr->tex[unit].w,dr->tex[unit].h,sample_pitch,rect);
}

static int rt_sub_find_interior(const D3D12DrawRecord* dr,unsigned unit)
{
    /* New interior-alias support is deliberately limited to format-compatible
     * linear RGBA8. Compressed/swizzled/depth/AA views retain their own paths. */
    if((dr->tex[unit].fmt&0xBF)!=0xA5 || dr->tex[unit].cube || dr->tex[unit].vol)return -1;
    if(off_rt_find(dr->tex[unit].raw)>=0)return -1; /* exact owner wins */
    int found=-1;
    for(int i=0;i<MAX_OFF_RTS;++i) {
        const OffRT* r=&s_d3d.off_rt[i];rsx_rt_rect rect;
        if(!r->res || r->dxgi!=DXGI_FORMAT_R8G8B8A8_UNORM || !rt_sub_rect(i,dr,unit,&rect))continue;
        if(found>=0)return -1; /* overlapping/ambiguous owners: do not guess */
        found=i;
    }
    return found;
}

static ID3D12Resource* rt_sub_source(int index,D3D12DrawRecord* dr,unsigned unit)
{
    OffRT* r=&s_d3d.off_rt[index];rsx_rt_rect rect;
    if(!rt_sub_rect(index,dr,unit,&rect) || (!rect.x&&!rect.y&&rect.w==r->w&&rect.h==r->h))return NULL;
    unsigned slot=MAX_RT_SUBREGIONS;
    for(unsigned i=0;i<MAX_RT_SUBREGIONS;++i)
        if(r->sub[i].res && !memcmp(&r->sub[i].rect,&rect,sizeof rect)){slot=i;break;}
    if(slot==MAX_RT_SUBREGIONS) {
        for(unsigned i=0;i<MAX_RT_SUBREGIONS;++i)if(!r->sub[i].res){slot=i;break;}
        if(slot==MAX_RT_SUBREGIONS) {
            static int shown;if(PS3_LOG_FIRST(shown,4))fprintf(stderr,"[rt-sub] region capacity reached for %08X\n",r->off);
            return NULL; /* Never evict a view referenced by an in-flight draw. */
        }
        RtSubregion* s=&r->sub[slot];
        D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td={0};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width=rect.w;td.Height=rect.h;td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;
        td.Format=(DXGI_FORMAT)r->dxgi;
        HRESULT hr=s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,
            &td,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,NULL,&IID_ID3D12Resource,(void**)&s->res);
        if(FAILED(hr)){s->res=NULL;return NULL;}
        s->rect=rect;s->state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;s->copied_generation=UINT64_MAX;
        static int shown;if(PS3_LOG_FIRST(shown,32))fprintf(stderr,
            "[rt-sub] off=%08X pitch=%u target=%ux%u region=%u,%u %ux%u slot=%u\n",
            r->off,r->pitch,r->w,r->h,rect.x,rect.y,rect.w,rect.h,slot);
    }
    dr->tex_rt_sub|=1u<<unit;dr->tex_rt_region[unit]=slot;
    return r->sub[slot].res;
}

static int rt_sub_copy(int index,unsigned slot)
{
    if(index<0 || slot>=MAX_RT_SUBREGIONS)return 0;
    OffRT* r=&s_d3d.off_rt[index];RtSubregion* s=&r->sub[slot];
    if(!s->res || s->copied_generation==r->generation)return 0;
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list,0,NULL,FALSE,NULL);
    off_rt_transition(index,D3D12_RESOURCE_STATE_COPY_SOURCE);
    zeta_barrier(s->res,&s->state,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst={0},src={0};
    dst.pResource=s->res;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource=r->res;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box={s->rect.x,s->rect.y,0,s->rect.x+s->rect.w,s->rect.y+s->rect.h,1};
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list,&dst,0,0,0,&src,&box);
    zeta_barrier(s->res,&s->state,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    off_rt_transition(index,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s->copied_generation=r->generation;
    return 1;
}

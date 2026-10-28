/* Included after s_d3d, srv_write and wait_for_gpu. GPU-only normalized-depth
 * -> guest Z24S8 bytes. Current backend renders ONE sample; the 2x/4x guest
 * storage footprint repeats that sample. This is not native MSAA coverage. */
#include "rsx_zeta_layout.h"
#include "rsx_d3d12_zeta_shader.h"
static int zeta_enabled(void){
    static int en=-1;if(en<0){const char*e=getenv("RSX_TRACK_ZETA");en=e&&e[0]=='1';}return en;
}
static D3D12_CPU_DESCRIPTOR_HANDLE zeta_dsv(int index){
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap,&h);
    h.ptr+=(SIZE_T)(index+1)*s_d3d.dsv_inc;
    return h;
}
static void zeta_barrier(ID3D12Resource*r,D3D12_RESOURCE_STATES*state,D3D12_RESOURCE_STATES next){
    if(*state==next)return;
    D3D12_RESOURCE_BARRIER b={0};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=r;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=*state;b.Transition.StateAfter=next;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list,1,&b);*state=next;
}
static int zeta_pack_init(void){
    if(s_d3d.zeta_pack_pso)return 1;
    return rsx_zeta_compile_pack(s_d3d.device,&s_d3d.zeta_pack_root,&s_d3d.zeta_pack_pso);
}
static int zeta_get(const D3D12DrawRecord*dr){
    if(!zeta_enabled()||!dr->zeta_raw||!dr->zeta_ea||dr->zeta_ea==0xffffffffu||
       !rsx_zeta_supported(dr->zeta_format)||!dr->zeta_w||!dr->zeta_h||dr->zeta_w>2048||dr->zeta_h>2048)return -1;
    int index=-1;
    for(int i=0;i<MAX_ZETA_RTS;++i){
        ZetaRT*r=&s_d3d.zeta[i];
        if(r->res&&r->ea==dr->zeta_ea&&r->w==dr->zeta_w&&r->h==dr->zeta_h&&r->pitch==dr->zeta_pitch&&r->format==dr->zeta_format){r->used=1;return i;}
        if(!r->res&&index<0)index=i;
    }
    if(index<0){static int reported;if(!reported){reported=1;fprintf(stderr,"[zeta] pool full; legacy fallback\n");}return -1;}
    if(!zeta_pack_init())return -1;
    ZetaRT*r=&s_d3d.zeta[index];
    r->ea=dr->zeta_ea;r->w=dr->zeta_w;r->h=dr->zeta_h;r->pitch=dr->zeta_pitch;r->format=dr->zeta_format;
    r->sx=rsx_zeta_scale_x(r->format);r->sy=rsx_zeta_scale_y(r->format);
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td={0};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width=r->w;td.Height=r->h;td.DepthOrArraySize=1;td.MipLevels=1;td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R24G8_TYPELESS;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv={0};cv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;cv.DepthStencil.Depth=1;
    HRESULT hr=s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &cv,&IID_ID3D12Resource,(void**)&r->res);
    if(FAILED(hr))goto failed;
    r->state=D3D12_RESOURCE_STATE_DEPTH_WRITE;
    td.Width*=r->sx;td.Height*=r->sy;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr=s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        NULL,&IID_ID3D12Resource,(void**)&r->packed);
    if(FAILED(hr))goto failed;
    r->packed_state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;r->used=1;r->dirty=1;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv={0};dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device,r->res,&dv,zeta_dsv(index));
    D3D12_CPU_DESCRIPTOR_HANDLE h;s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap,&h);
    h.ptr+=(SIZE_T)(ZETA_SRV_BASE+index*3)*s_d3d.srv_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv={0};sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels=1;sv.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device,r->res,&sv,h);h.ptr+=s_d3d.srv_inc;
    sv.Format=DXGI_FORMAT_X24_TYPELESS_G8_UINT;sv.Texture2D.PlaneSlice=1;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device,r->res,&sv,h);h.ptr+=s_d3d.srv_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv={0};uv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    s_d3d.device->lpVtbl->CreateUnorderedAccessView(s_d3d.device,r->packed,NULL,&uv,h);
    fprintf(stderr,"[zeta] slot=%d ea=%08X %ux%u guest_grid=%ux%u (native samples=1)\n",index,r->ea,r->w,r->h,r->sx,r->sy);
    return index;
failed:
    fprintf(stderr,"[zeta] creation failed %08lX\n",hr);
    if(r->res)r->res->lpVtbl->Release(r->res);if(r->packed)r->packed->lpVtbl->Release(r->packed);
    memset(r,0,sizeof *r);return -1;
}
static int zeta_alias_find(u32 ea,u32 w,u32 h,u32 fmt,u32 pitch){
    if((fmt&0x9fu)!=0x85)return -1;
    for(int i=0;i<MAX_ZETA_RTS;++i){ZetaRT*r=&s_d3d.zeta[i];
        if(r->used&&r->res&&r->ea==ea&&w==r->w*r->sx&&h==r->h*r->sy&&(!pitch||pitch==r->pitch))return i;}
    return -1;
}
/* Returns whether it changed OM binding / pipeline. No CPU readback or waits. */
static int zeta_pack(int index){
    ZetaRT*r=&s_d3d.zeta[index];if(!r->dirty)return 0;
    {static int enabled=-1;static u64 last_frame=~0ull;static unsigned count,frames;
        if(enabled<0){const char*e=getenv("RSX_ZETA_STATS");enabled=e&&e[0]=='1';}
        if(enabled){
            if(last_frame!=s_d3d.frame_count){
                if(last_frame!=~0ull && (++frames<8 || frames%240==0))
                    fprintf(stderr,"[zeta-stats] frame=%llu conversions=%u\n",(unsigned long long)last_frame,count);
                last_frame=s_d3d.frame_count;count=0;
            }
            ++count;
        }
    }
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list,0,NULL,FALSE,NULL);
    if(!r->initialized){
        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list,zeta_dsv(index),D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,1,0,0,NULL);
        r->initialized=1;
    }
    zeta_barrier(r->res,&r->state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    zeta_barrier(r->packed,&r->packed_state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s_d3d.cmd_list->lpVtbl->SetComputeRootSignature(s_d3d.cmd_list,s_d3d.zeta_pack_root);
    s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list,s_d3d.zeta_pack_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE h;s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap,&h);
    h.ptr+=(UINT64)(ZETA_SRV_BASE+index*3)*s_d3d.srv_inc;
    s_d3d.cmd_list->lpVtbl->SetComputeRootDescriptorTable(s_d3d.cmd_list,0,h);h.ptr+=2*s_d3d.srv_inc;
    s_d3d.cmd_list->lpVtbl->SetComputeRootDescriptorTable(s_d3d.cmd_list,1,h);
    u32 values[4]={r->sx,r->sy,r->w*r->sx,r->h*r->sy};
    s_d3d.cmd_list->lpVtbl->SetComputeRoot32BitConstants(s_d3d.cmd_list,2,4,values,0);
    s_d3d.cmd_list->lpVtbl->Dispatch(s_d3d.cmd_list,(values[2]+15)/16,(values[3]+15)/16,1);
    zeta_barrier(r->packed,&r->packed_state,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    zeta_barrier(r->res,&r->state,D3D12_RESOURCE_STATE_DEPTH_WRITE);r->dirty=0;r->packed_valid=1;return 1;
}

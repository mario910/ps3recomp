/* Logical-pixel raster -> guest AA storage. This repairs the texture's extent
 * contract, NOT MSAA coverage: the current backend still rasterizes one sample.
 * GPU-only, lazy on actual FP reads; no CPU readback or shader bypass. */
#include "rsx_d3d12_msaa_shader.h"
static ID3D12RootSignature*s_color_grid_root;
static ID3D12PipelineState*s_color_grid_pso;
static ID3D12Resource*color_grid_source(int index,D3D12DrawRecord*dr,unsigned unit){
    OffRT*r=&s_d3d.off_rt[index];
    if(!color_grid_enabled() || !(dr->fp_texture_mask&(1u<<unit)) || r->sx*r->sy==1 ||
       r->dxgi!=DXGI_FORMAT_R8G8B8A8_UNORM || dr->tex[unit].w!=r->w*r->sx || dr->tex[unit].h!=r->h*r->sy)
        return r->res;
    if(!s_color_grid_pso && !rsx_msaa_compile_color_pack(s_d3d.device,&s_color_grid_root,&s_color_grid_pso,1))return r->res;
    if(!r->packed){
        D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td={0};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width=r->w*r->sx;td.Height=r->h*r->sy;td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;
        td.Format=(DXGI_FORMAT)r->dxgi;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr=s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,&td,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,NULL,&IID_ID3D12Resource,(void**)&r->packed);
        if(FAILED(hr)){fprintf(stderr,"[color-grid] creation failed %08lX\n",hr);return r->res;}
        r->packed_state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;r->packed_dirty=1;
        srv_write(COLOR_GRID_SRV_BASE+index*2,r->res,(DXGI_FORMAT)r->dxgi,D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        D3D12_CPU_DESCRIPTOR_HANDLE h;s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap,&h);
        h.ptr+=(SIZE_T)(COLOR_GRID_SRV_BASE+index*2+1)*s_d3d.srv_inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv={0};uv.Format=(DXGI_FORMAT)r->dxgi;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        s_d3d.device->lpVtbl->CreateUnorderedAccessView(s_d3d.device,r->packed,NULL,&uv,h);
        fprintf(stderr,"[color-grid] off=%08X raster=%ux%u texture=%ux%u native_samples=1\n",r->off,r->w,r->h,r->w*r->sx,r->h*r->sy);
    }
    dr->tex_color_grid|=1u<<unit;return r->packed;
}
static int color_grid_pack(int index){
    if(index<0)return 0;OffRT*r=&s_d3d.off_rt[index];if(!r->packed || !r->packed_dirty)return 0;
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list,0,NULL,FALSE,NULL);
    off_rt_transition(index,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    zeta_barrier(r->packed,&r->packed_state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s_d3d.cmd_list->lpVtbl->SetComputeRootSignature(s_d3d.cmd_list,s_color_grid_root);
    s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list,s_color_grid_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE h;s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap,&h);
    h.ptr+=(UINT64)(COLOR_GRID_SRV_BASE+index*2)*s_d3d.srv_inc;
    s_d3d.cmd_list->lpVtbl->SetComputeRootDescriptorTable(s_d3d.cmd_list,0,h);h.ptr+=s_d3d.srv_inc;
    s_d3d.cmd_list->lpVtbl->SetComputeRootDescriptorTable(s_d3d.cmd_list,1,h);
    u32 values[]={r->sx,r->sy,r->w*r->sx,r->h*r->sy};
    s_d3d.cmd_list->lpVtbl->SetComputeRoot32BitConstants(s_d3d.cmd_list,2,4,values,0);
    s_d3d.cmd_list->lpVtbl->Dispatch(s_d3d.cmd_list,(values[2]+15)/16,(values[3]+15)/16,1);
    zeta_barrier(r->packed,&r->packed_state,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    off_rt_transition(index,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);r->packed_dirty=0;return 1;
}

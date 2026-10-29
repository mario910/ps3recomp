#ifndef RSX_D3D12_MSAA_SHADER_H
#define RSX_D3D12_MSAA_SHADER_H
#include "rsx_d3d12_pack_compile.h"
/* Preserve guest sample storage, NOT a resolve average. Later shaders may
 * read neighboring samples as neighboring texels in the guest AA footprint. */
static int rsx_msaa_compile_color_pack(ID3D12Device*device,ID3D12RootSignature**root,ID3D12PipelineState**pipeline,unsigned samples){
    static const char shader[]=
        "#if SAMPLES == 1\nTexture2D<float4> C:register(t0);\n#else\nTexture2DMS<float4,SAMPLES> C:register(t0);\n#endif\n"
        "RWTexture2D<float4> P:register(u0);"
        "cbuffer K:register(b0){uint2 scale;uint2 extent;}"
        "[numthreads(16,16,1)] void main(uint3 id:SV_DispatchThreadID){"
        "if(any(id.xy>=extent))return;int2 pos=int2(id.xy/scale);"
        "uint sample=(id.x%scale.x)+(id.y%scale.y)*scale.x;\n"
        "#if SAMPLES == 1\nP[id.xy]=C.Load(int3(pos,0));\n#else\nP[id.xy]=C.Load(pos,sample);\n#endif\n}";
    *root=NULL;*pipeline=NULL;if(samples!=1 && samples!=2 && samples!=4)return 0;
    D3D_SHADER_MACRO macros[]={{"SAMPLES",samples==1?"1":samples==2?"2":"4"},{NULL,NULL}};
    return rsx_d3d12_compile_pack_shader(device,root,pipeline,shader,sizeof shader-1,macros,1,"color_sample_pack");
}
#endif

#ifndef RSX_D3D12_ZETA_SHADER_H
#define RSX_D3D12_ZETA_SHADER_H
/* Included after d3d12.h/d3dcompiler.h; also used by the standalone GPU test. */
#include "rsx_d3d12_pack_compile.h"
static int rsx_zeta_compile_pack_samples(ID3D12Device* device,ID3D12RootSignature**root,ID3D12PipelineState**pipeline,unsigned samples){
    static const char shader[]=
        "#if SAMPLES > 1\n"
        "Texture2DMS<float,SAMPLES> Z:register(t0); Texture2DMS<uint2,SAMPLES> S:register(t1);\n"
        "#else\nTexture2D<float> Z:register(t0); Texture2D<uint2> S:register(t1);\n#endif\n"
        "RWTexture2D<float4> P:register(u0); cbuffer C:register(b0){uint2 scale;uint2 extent;}"
        "[numthreads(16,16,1)] void main(uint3 id:SV_DispatchThreadID){"
        "if(any(id.xy>=extent))return; int2 pos=int2(id.xy/scale); float depth; uint s;\n"
        "#if SAMPLES > 1\nuint sample=(id.x%scale.x)+(id.y%scale.y)*scale.x;"
        "depth=Z.Load(pos,sample);s=S.Load(pos,sample).g;\n"
        "#else\ndepth=Z.Load(int3(pos,0));s=S.Load(int3(pos,0)).g;\n#endif\n"
        "uint z=(uint)round(saturate(depth)*16777215.0);"
        "P[id.xy]=float4((z>>8)&255,z&255,s&255,(z>>16)&255)/255.0;}";
    *root=NULL;*pipeline=NULL;
    if(samples!=1 && samples!=2 && samples!=4)return 0;
    D3D_SHADER_MACRO macros[]={{"SAMPLES",samples==1?"1":samples==2?"2":"4"},{NULL,NULL}};
    return rsx_d3d12_compile_pack_shader(device,root,pipeline,shader,sizeof shader-1,macros,2,"zeta_pack");
}
static int rsx_zeta_compile_pack(ID3D12Device* device,ID3D12RootSignature**root,ID3D12PipelineState**pipeline){
    return rsx_zeta_compile_pack_samples(device,root,pipeline,1);
}
#endif

#ifndef RSX_D3D12_VERTEX_REFLECTION_H
#define RSX_D3D12_VERTEX_REFLECTION_H
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <stdint.h>
#include <string.h>
/* Use the compiled interface, not an enabled-array or dimension heuristic.
 * Unknown semantics/failed reflection conservatively upload every attribute. */
static inline uint32_t rsx_d3d12_vertex_inputs(const void* bytecode,SIZE_T bytes)
{
    ID3D11ShaderReflection* reflection=NULL;
    if(FAILED(D3DReflect(bytecode,bytes,&IID_ID3D11ShaderReflection,(void**)&reflection)))return 0xffffu;
    D3D11_SHADER_DESC shader;
    uint32_t mask=0;
    if(FAILED(reflection->lpVtbl->GetDesc(reflection,&shader)))mask=0xffffu;
    else for(UINT n=0;n<shader.InputParameters;++n) {
        D3D11_SIGNATURE_PARAMETER_DESC parameter;
        if(FAILED(reflection->lpVtbl->GetInputParameterDesc(reflection,n,&parameter))) {mask=0xffffu;break;}
        if(parameter.SystemValueType!=D3D_NAME_UNDEFINED)continue;
        if(!parameter.SemanticName || strcmp(parameter.SemanticName,"ATTR") || parameter.SemanticIndex>=16) {mask=0xffffu;break;}
        mask|=1u<<parameter.SemanticIndex;
    }
    reflection->lpVtbl->Release(reflection);
    return mask?mask:0xffffu;
}
#endif

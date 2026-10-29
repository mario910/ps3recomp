#ifndef RSX_D3D12_PACK_COMPILE_H
#define RSX_D3D12_PACK_COMPILE_H
#include <stdio.h>
static int rsx_d3d12_compile_pack_shader(ID3D12Device*device,ID3D12RootSignature**root,
    ID3D12PipelineState**pipeline,const char*shader,size_t bytes,const D3D_SHADER_MACRO*macros,unsigned srvs,const char*name){
    *root=NULL;*pipeline=NULL;
    D3D12_DESCRIPTOR_RANGE ranges[2]={{0}};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=srvs;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=1;
    D3D12_ROOT_PARAMETER params[3]={{0}};
    for(unsigned n=0;n<2;++n){params[n].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[n].DescriptorTable.NumDescriptorRanges=1;params[n].DescriptorTable.pDescriptorRanges=&ranges[n];}
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants.Num32BitValues=4;
    D3D12_ROOT_SIGNATURE_DESC desc={0};desc.NumParameters=3;desc.pParameters=params;
    ID3DBlob*blob=NULL,*errors=NULL,*code=NULL;
    HRESULT hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);
    if(errors)errors->lpVtbl->Release(errors);
    if(FAILED(hr)||!blob)return 0;
    hr=device->lpVtbl->CreateRootSignature(device,0,blob->lpVtbl->GetBufferPointer(blob),blob->lpVtbl->GetBufferSize(blob),
                                          &IID_ID3D12RootSignature,(void**)root);
    blob->lpVtbl->Release(blob);if(FAILED(hr))return 0;
    errors=NULL;hr=D3DCompile(shader,bytes,name,macros,NULL,"main","cs_5_0",0,0,&code,&errors);
    if(FAILED(hr))fprintf(stderr,"[%s] compile failed: %s\n",name,errors?(char*)errors->lpVtbl->GetBufferPointer(errors):"unknown");
    if(errors)errors->lpVtbl->Release(errors);
    if(FAILED(hr)||!code){(*root)->lpVtbl->Release(*root);*root=NULL;return 0;}
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd={0};pd.pRootSignature=*root;
    pd.CS.pShaderBytecode=code->lpVtbl->GetBufferPointer(code);pd.CS.BytecodeLength=code->lpVtbl->GetBufferSize(code);
    hr=device->lpVtbl->CreateComputePipelineState(device,&pd,&IID_ID3D12PipelineState,(void**)pipeline);
    code->lpVtbl->Release(code);
    if(FAILED(hr)){(*root)->lpVtbl->Release(*root);*root=NULL;return 0;}
    return 1;
}
#endif

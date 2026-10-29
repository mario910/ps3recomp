#ifndef RSX_D3D12_SAMPLER_H
#define RSX_D3D12_SAMPLER_H
#include <stdint.h>
/* Include after d3d12.h. Same NV4097 fields as the live-draw/Metal decoders.
 * Anisotropy and legacy GL CLAMP/mirror-border distinctions remain unsupported. */
typedef struct { uint32_t filter, address, control0, border; } RsxSamplerKey;
static inline RsxSamplerKey rsx_sampler_key(uint32_t f,uint32_t a,uint32_t c,uint32_t b)
{
    RsxSamplerKey k={f&0x07071fffu,a&0x000f0f0fu,c&0x7fffff80u,b};
    return k;
}
static inline D3D12_TEXTURE_ADDRESS_MODE rsx_sampler_wrap(uint32_t mode)
{
    switch(mode&15u) {
    case 1:return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 2:return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 4:return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 6:case 7:case 8:return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default:return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}
static inline D3D12_SAMPLER_DESC rsx_sampler_decode(RsxSamplerKey k)
{
    D3D12_SAMPLER_DESC d={0};
    uint32_t mn=(k.filter>>16)&7u, mg=(k.filter>>24)&7u;
    d.Filter=D3D12_ENCODE_BASIC_FILTER(
        (mn==2 || mn==4 || mn==6) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        mg==2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        (mn==5 || mn==6) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    d.AddressU=rsx_sampler_wrap(k.address);
    d.AddressV=rsx_sampler_wrap(k.address>>8);
    d.AddressW=rsx_sampler_wrap(k.address>>16);
    int bias=(int)(k.filter&0x1fffu);
    if(bias&0x1000) bias-=0x2000;
    d.MipLODBias=(float)bias/256.0f;
    if(d.MipLODBias>15.99f)d.MipLODBias=15.99f;
    d.MinLOD=(float)((k.control0>>19)&0xfffu)/256.0f;
    d.MaxLOD=mn>=3 ? (float)((k.control0>>7)&0xfffu)/256.0f : 0;
    if(d.MaxLOD<d.MinLOD)d.MaxLOD=d.MinLOD;
    d.MaxAnisotropy=1;
    d.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    d.BorderColor[0]=(float)((k.border>>16)&255)/255.0f;
    d.BorderColor[1]=(float)((k.border>>8)&255)/255.0f;
    d.BorderColor[2]=(float)(k.border&255)/255.0f;
    d.BorderColor[3]=(float)(k.border>>24)/255.0f;
    return d;
}
#endif

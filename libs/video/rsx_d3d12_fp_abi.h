#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* Code includes inline 16-byte constants. Real Buzz shaders exceed 4 KiB.
 * Keep capture bounds, CPU upload and HLSL declarations on one contract. */
#define RSX_D3D12_FP_CODE_BYTES 16384u
#define RSX_D3D12_FP_CONSTANTS 256u
#define RSX_D3D12_FP_CB_BYTES (((5u+RSX_D3D12_FP_CONSTANTS)*16u+255u)&~255u)
static inline uint32_t rsx_d3d12_fp_guest_bound(uint32_t address) {
    uint64_t remaining=0x100000000ull-address;
    return remaining<RSX_D3D12_FP_CODE_BYTES?(uint32_t)remaining:RSX_D3D12_FP_CODE_BYTES;
}
static inline int rsx_d3d12_fp_probe_matches(const char* list,uint32_t hash)
{
    if (!list) return 0;
    if (!strcmp(list,"all")) return 1;
    while (*list) {
        char* end;
        unsigned long value=strtoul(list,&end,16);
        if (end==list || (*end && *end!=',')) return 0;
        if (value==hash) return 1;
        if (!*end) break;
        list=end+1;
    }
    return 0;
}

/* Diagnostic only: prove rasterization independently of texture reads,
 * inline constants, KIL instructions and dynamic alpha-test state. */
static inline int rsx_d3d12_fp_debug_color(char* hlsl,uint32_t capacity,
                                          float r,float g,float b)
{
    char* main_function=strstr(hlsl," main(PSInput input)");
    char* body=main_function?strchr(main_function,'{'):NULL;
    if (!body) return -1;
    ++body;
    char prefix[256];
    int n;
    if (strstr(hlsl,"struct PSOutput"))
        n=snprintf(prefix,sizeof prefix,"\n    PSOutput _probe=(PSOutput)0; _probe.t0=float4(%.9g,%.9g,%.9g,1); return _probe;\n",r,g,b);
    else n=snprintf(prefix,sizeof prefix,"\n    return float4(%.9g,%.9g,%.9g,1);\n",r,g,b);
    if (n<0 || (size_t)n>=sizeof prefix || strlen(hlsl)+(size_t)n+1>capacity) return -1;
    memmove(body+n,body,strlen(body)+1);
    memcpy(body,prefix,(size_t)n);
    return 0;
}

/* The legacy D3D12 uploader uses b1 vectors 0..3 for texture scales, 4 for
 * alpha-test state and 5 onward for inline constants. The current decompiler's
 * generic buffered layout starts constants at vector zero. Adapt its actual
 * declaration, not the shader's values, so each draw reads its own snapshot. */
static inline int rsx_d3d12_fp_bind_constants(char* hlsl, uint32_t capacity,
                                             uint32_t constant_count)
{
    if (!hlsl || constant_count > RSX_D3D12_FP_CONSTANTS) return -1;
    char* first = strstr(hlsl, "cbuffer PSConstants : register(b1) {");
    char* last = first ? strstr(first, "};\n") : hlsl;
    if (first && !last) return -1;
    if (first) last += 3;
    else first = hlsl;
    char declaration[256];
    int length=snprintf(declaration,sizeof declaration,
        "cbuffer PSConstants : register(b1) {\n"
        "    float4 rsx_texscale[4];\n"
        "    float4 rsx_alphatest;\n"
        "    float4 fp_constants[%u];\n"
        "};\n",RSX_D3D12_FP_CONSTANTS);
    if(length<0 || (size_t)length>=sizeof declaration)return -1;
    const size_t old_size = (size_t)(last - first);
    const size_t new_size = (size_t)length;
    const size_t used = strlen(hlsl);
    if (used - old_size + new_size + 1 > capacity) return -1;
    memmove(first + new_size, last, strlen(last) + 1);
    memcpy(first, declaration, new_size);
    return 0;
}

/* The shader cache does not key alpha enable/function/ref: they belong to
 * the draw, just like the inline colors. Evaluate all eight comparisons from
 * b1 instead of baking the first draw's alpha test into its shared PSO. */
static inline int rsx_d3d12_fp_alpha_test(char* hlsl, uint32_t capacity)
{
    if (!hlsl) return -1;
    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p; p = strstr(p + 1, "    return ")) marker = p;
    if (!marker) return -1;
    const char* value = marker + strlen("    return ");
    const char* end = strstr(value, ";\n}\n");
    if (!end || end == value || end - value >= 96) return -1;
    char expression[96]; memcpy(expression,value,(size_t)(end-value)); expression[end-value]=0;
    int mrt = strstr(hlsl,"struct PSOutput") != NULL;
    char suffix[1400];
    int n = snprintf(suffix,sizeof suffix,
        "    %s _rsx_draw_result = %s;\n"
        "    float _rsx_a = %s.a, _rsx_ref = rsx_alphatest.y;\n"
        "    int _rsx_func = (int)rsx_alphatest.z;\n"
        "    bool _rsx_pass = _rsx_func == 7 || (_rsx_func == 1 && _rsx_a < _rsx_ref) ||\n"
        "        (_rsx_func == 2 && _rsx_a == _rsx_ref) || (_rsx_func == 3 && _rsx_a <= _rsx_ref) ||\n"
        "        (_rsx_func == 4 && _rsx_a > _rsx_ref) || (_rsx_func == 5 && _rsx_a != _rsx_ref) ||\n"
        "        (_rsx_func == 6 && _rsx_a >= _rsx_ref);\n"
        "    if (rsx_alphatest.x != 0 && !_rsx_pass) discard;\n"
        "    return _rsx_draw_result;\n}\n", mrt?"PSOutput":"float4", expression,
        mrt?"_rsx_draw_result.t0":"_rsx_draw_result");
    if (n < 0 || (size_t)n >= sizeof suffix || (size_t)(marker-hlsl)+(size_t)n+1 > capacity) return -1;
    memcpy(marker,suffix,(size_t)n+1);
    return 0;
}

/* Scale only a 2D sample's coordinate argument; leave LOD/bias arguments and
 * cubemap directions untouched. Supports both sampler spellings emitted by
 * the current decompiler and all three sampling operations. */
static inline int rsx_d3d12_fp_texture_scales(char* hlsl, uint32_t capacity, uint32_t cube_mask)
{
    static const char* operations[] = {"Sample", "SampleLevel", "SampleBias"};
    int count = 0;
    for (unsigned u = 0; u < 4; ++u) {
        if (cube_mask & (1u << u)) continue;
        for (unsigned op = 0; op < 3; ++op) for (unsigned spelling = 0; spelling < 2; ++spelling) {
            char needle[80], suffix[64];
            if (spelling) snprintf(needle,sizeof needle,"rsx_tex%u.%s(rsx_samp[%u],",u,operations[op],u);
            else snprintf(needle,sizeof needle,"rsx_tex[%u].%s(rsx_samp[%u],",u,operations[op],u);
            snprintf(suffix,sizeof suffix,") * rsx_texscale[%u].xy",u);
            char* search = hlsl;
            for (char* at; (at=strstr(search,needle)) != NULL;) {
                char* coordinate=at+strlen(needle), *end=coordinate;
                int depth=0;
                for (; *end; ++end) {
                    if (*end=='(') ++depth;
                    else if (*end==')') { if (!depth) break; --depth; }
                    else if (*end==',' && !depth) break;
                }
                if (!*end) return -1;
                size_t extra=1+strlen(suffix), used=strlen(hlsl), length=(size_t)(end-coordinate);
                if (used+extra+1>capacity) return -1;
                memmove(end+extra,end,strlen(end)+1);
                memmove(coordinate+1,coordinate,length);
                *coordinate='(';
                memcpy(end+1,suffix,strlen(suffix));
                search=end+extra; ++count;
            }
        }
    }
    return count;
}

/*
 * ps3recomp - FSR 1 presentation upscaler (EASU + RCAS), shared HLSL
 *
 * A compact port of AMD FidelityFX Super Resolution 1.0: EASU, the
 * edge-adaptive spatial upscale, then RCAS, robust contrast-adaptive
 * sharpening. Algorithm (c) 2021 Advanced Micro Devices, Inc., MIT licence;
 * see THIRD_PARTY_NOTICES.md. 32-bit float throughout, texel loads only (no
 * sampler, no gather) so one source serves D3DCompile and glslang alike.
 *
 * FidelityFX Super Resolution 1.0 algorithm:
 * Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Both passes run on the finished frame, after all guest rendering: EASU
 * from the internal-resolution frame into an intermediate image the size of
 * the letterboxed output rectangle, RCAS from that image into the window.
 * Used only when the output is larger than the internal resolution.
 *
 * Constants, one block of two float4 (push constants on Vulkan, root
 * constants b0 on D3D12):
 *   sizes  = (input width, input height, output width, output height)
 *   params = (RCAS sharpness as exp2(-stops), unused, output origin x, y)
 * SV_Position minus the origin is the pixel within the output rectangle.
 */
#ifndef PS3RECOMP_RSX_FSR1_H
#define PS3RECOMP_RSX_FSR1_H
#include <math.h>

#define RSX_FSR1_COMMON \
    "#ifdef RSX_VK\n" \
    "[[vk::push_constant]] cbuffer C { float4 sizes; float4 params; };\n" \
    "#else\n" \
    "cbuffer C : register(b0) { float4 sizes; float4 params; };\n" \
    "#endif\n" \
    "Texture2D src : register(t0);\n" \
    "struct V { float4 p : SV_POSITION; };\n" \
    "float3 ld(int2 p, int2 extent) { return src.Load(int3(clamp(p, int2(0, 0), extent - 1), 0)).rgb; }\n"

/* Full-screen triangle; the passes work from SV_Position alone. */
static const char rsx_fsr1_vs[] =
    "struct V { float4 p : SV_POSITION; };\n"
    "V main(uint id : SV_VertexID) { V o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";

static const char rsx_fsr1_easu_ps[] =
    RSX_FSR1_COMMON
    "float luma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
    "void edge(inout float2 dir, inout float len, float w,\n"
    "          float la, float lb, float lc, float ld_, float le) {\n"
    "  float lenx = max(abs(ld_ - lc), abs(lc - lb));\n"
    "  float dirx = ld_ - lb;\n"
    "  lenx = saturate(abs(dirx) / max(lenx, 1.0 / 65536.0)); lenx *= lenx;\n"
    "  float leny = max(abs(le - lc), abs(lc - la));\n"
    "  float diry = le - la;\n"
    "  leny = saturate(abs(diry) / max(leny, 1.0 / 65536.0)); leny *= leny;\n"
    "  dir += float2(dirx, diry) * w;\n"
    "  len += (lenx + leny) * w;\n"
    "}\n"
    "void tap(inout float3 ac, inout float aw, float2 off, float2 dir, float2 len2,\n"
    "         float lob, float clp, float3 c) {\n"
    "  float2 v = float2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;\n"
    "  float d2 = min(dot(v, v), clp);\n"
    "  float wb = 2.0 / 5.0 * d2 - 1.0; float wa = lob * d2 - 1.0;\n"
    "  wb *= wb; wa *= wa; wb = 25.0 / 16.0 * wb - (25.0 / 16.0 - 1.0);\n"
    "  float w = wb * wa; ac += c * w; aw += w;\n"
    "}\n"
    "float4 main(V i) : SV_TARGET {\n"
    "  int2 extent = int2(sizes.xy);\n"
    "  float2 scale = sizes.xy / sizes.zw;\n"
    "  float2 pp = (floor(i.p.xy - params.zw) + 0.5) * scale - 0.5;\n"
    "  float2 fp = floor(pp); pp -= fp; int2 b0 = int2(fp);\n"
    "  float3 b = ld(b0 + int2(0, -1), extent), c = ld(b0 + int2(1, -1), extent);\n"
    "  float3 e = ld(b0 + int2(-1, 0), extent), f = ld(b0, extent);\n"
    "  float3 g = ld(b0 + int2(1, 0), extent), h = ld(b0 + int2(2, 0), extent);\n"
    "  float3 ii = ld(b0 + int2(-1, 1), extent), j = ld(b0 + int2(0, 1), extent);\n"
    "  float3 k = ld(b0 + int2(1, 1), extent), l = ld(b0 + int2(2, 1), extent);\n"
    "  float3 n = ld(b0 + int2(0, 2), extent), o = ld(b0 + int2(1, 2), extent);\n"
    "  float bl = luma(b), cl = luma(c), el = luma(e), fl = luma(f), gl = luma(g), hl = luma(h);\n"
    "  float il = luma(ii), jl = luma(j), kl = luma(k), ll = luma(l), nl = luma(n), ol = luma(o);\n"
    "  float2 dir = 0; float len = 0;\n"
    "  edge(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bl, el, fl, gl, jl);\n"
    "  edge(dir, len, pp.x * (1.0 - pp.y), cl, fl, gl, hl, kl);\n"
    "  edge(dir, len, (1.0 - pp.x) * pp.y, fl, il, jl, kl, nl);\n"
    "  edge(dir, len, pp.x * pp.y, gl, jl, kl, ll, ol);\n"
    "  float dirr = dot(dir, dir);\n"
    "  bool zero = dirr < 1.0 / 32768.0;\n"
    "  dirr = zero ? 1.0 : rsqrt(dirr);\n"
    "  dir.x = zero ? 1.0 : dir.x;\n"
    "  dir *= dirr;\n"
    "  len = len * 0.5; len *= len;\n"
    "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
    "  float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "  float lob = 0.5 + (1.0 / 4.0 - 0.04 - 0.5) * len;\n"
    "  float clp = 1.0 / lob;\n"
    "  float3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));\n"
    "  float3 ac = 0; float aw = 0;\n"
    "  tap(ac, aw, float2( 0, -1) - pp, dir, len2, lob, clp, b);\n"
    "  tap(ac, aw, float2( 1, -1) - pp, dir, len2, lob, clp, c);\n"
    "  tap(ac, aw, float2(-1,  1) - pp, dir, len2, lob, clp, ii);\n"
    "  tap(ac, aw, float2( 0,  1) - pp, dir, len2, lob, clp, j);\n"
    "  tap(ac, aw, float2( 0,  0) - pp, dir, len2, lob, clp, f);\n"
    "  tap(ac, aw, float2(-1,  0) - pp, dir, len2, lob, clp, e);\n"
    "  tap(ac, aw, float2( 1,  1) - pp, dir, len2, lob, clp, k);\n"
    "  tap(ac, aw, float2( 2,  1) - pp, dir, len2, lob, clp, l);\n"
    "  tap(ac, aw, float2( 2,  0) - pp, dir, len2, lob, clp, h);\n"
    "  tap(ac, aw, float2( 1,  0) - pp, dir, len2, lob, clp, g);\n"
    "  tap(ac, aw, float2( 1,  2) - pp, dir, len2, lob, clp, o);\n"
    "  tap(ac, aw, float2( 0,  2) - pp, dir, len2, lob, clp, n);\n"
    "  return float4(clamp(ac / max(aw, 1.0 / 65536.0), mn, mx), 1.0);\n"
    "}\n";

/* RCAS: a 5-tap cross whose negative lobe is limited so it never pushes a
 * pixel outside its neighbours' range (FSR_RCAS_LIMIT = 1/4 - 1/16). */
static const char rsx_fsr1_rcas_ps[] =
    RSX_FSR1_COMMON
    "float4 main(V i) : SV_TARGET {\n"
    "  int2 extent = int2(sizes.zw);\n"
    "  int2 p = int2(i.p.xy - params.zw);\n"
    "  float3 b = ld(p + int2(0, -1), extent), d = ld(p + int2(-1, 0), extent);\n"
    "  float3 e = ld(p, extent);\n"
    "  float3 f = ld(p + int2(1, 0), extent), h = ld(p + int2(0, 1), extent);\n"
    "  float3 mn = min(min(b, d), min(f, h)), mx = max(max(b, d), max(f, h));\n"
    "  float3 hitmin = min(mn, e) / max(4.0 * mx, 1.0 / 65536.0);\n"
    "  float3 hitmax = (1.0 - max(mx, e)) / min(4.0 * mn - 4.0, -1.0 / 65536.0);\n"
    "  float3 lobergb = max(-hitmin, hitmax);\n"
    "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobergb.r, max(lobergb.g, lobergb.b)), 0.0)) * params.x;\n"
    "  float3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);\n"
    "  return float4(saturate(c), 1.0);\n"
    "}\n";

/* params.x for a sharpness setting of 0..100: 100 is RCAS at 0 stops (the
 * strongest), 0 is 2 stops (barely any). */
static float rsx_fsr1_sharpness(unsigned percent)
{
    if (percent > 100) percent = 100;
    const float stops = (float)(100 - percent) / 50.0f;
    return powf(2.0f, -stops);
}

#endif /* PS3RECOMP_RSX_FSR1_H */

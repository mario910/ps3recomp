#pragma once
#include <dxgiformat.h>
#include "rsx_texture_layout.h"

/* The decoded format, not the guest's packed storage format, determines the
 * D3D resource AND its SRV. Keep both call sites on the same mapping. */
static inline DXGI_FORMAT rsx_d3d12_texture_format(rsx_texfmt format)
{
    switch (format) {
    case RSX_TEXFMT_R8: return DXGI_FORMAT_R8_UNORM;
    case RSX_TEXFMT_R8G8: return DXGI_FORMAT_R8G8_UNORM;
    case RSX_TEXFMT_R8G8B8A8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RSX_TEXFMT_BC1: return DXGI_FORMAT_BC1_UNORM;
    case RSX_TEXFMT_BC2: return DXGI_FORMAT_BC2_UNORM;
    case RSX_TEXFMT_BC3: return DXGI_FORMAT_BC3_UNORM;
    case RSX_TEXFMT_R16: return DXGI_FORMAT_R16_UNORM;
    case RSX_TEXFMT_R16G16: return DXGI_FORMAT_R16G16_UNORM;
    case RSX_TEXFMT_R16G16F: return DXGI_FORMAT_R16G16_FLOAT;
    case RSX_TEXFMT_R16G16B16A16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RSX_TEXFMT_R32F: return DXGI_FORMAT_R32_FLOAT;
    case RSX_TEXFMT_R32G32B32A32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

/* Unknown guest formats must not silently retain a previous binding. The
 * portable decoder's unknown-format fallback to R8 is not proof of support. */
static inline int rsx_d3d12_texture_supported(u32 format)
{
    switch (format & 0x9fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8b: case 0x8c: case 0x8d:
    case 0x90: case 0x92: case 0x94: case 0x95: case 0x97:
    case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f:
        return 1;
    default: return 0;
    }
}

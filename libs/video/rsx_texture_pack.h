/*
 * ps3recomp - RSX texture packs: dump guest textures, load replacements
 *
 * A texture is named by what it is, not where it is: a 64-bit hash of its
 * level-0 guest bytes plus its size and format byte,
 *
 *     <hash:16 hex>_<w>x<h>_<format:2 hex>.png
 *
 * so the same image gets the same name in every run, at every address, on
 * every backend. Dump mode writes each new 2D colour texture to
 * <pack>/dump/<name>; a PNG of that name anywhere under <pack>/replace/ is
 * loaded instead of the guest's texels, at whatever size it has. Upscaled
 * packs are made offline from the dump (Real-ESRGAN and the like, or by hand).
 *
 * Only what can be swapped without the game noticing is eligible: 2D, not
 * cube, not volume, and a format that decodes to 8-bit channels (B8, G8B8,
 * the ARGB family, DXT1/23/45). A bigger texture samples the same through
 * normalised coordinates, and unnormalised ones are divided by the guest's
 * registered size in the shader (rsx_fp_apply_unnorm_scale), not the
 * resource's. Depth, float and 16-bit formats, which are read by position
 * or compared, are never touched. A surface the game renders and then
 * samples is new bytes every frame: it never matches a replacement, and an
 * address whose contents keep changing stops being dumped after a few.
 *
 * The replacement keeps the decoded channel layout the backend expects, so
 * the guest's component remap applies to it unchanged: RGBA for the ARGB
 * family and DXT (whose blocks are decoded on the CPU), one channel for B8,
 * two for G8B8. Its full mip chain is built by box filtering.
 *
 * replace/ is rescanned when ps3_host_texture_pack_changed() is signalled,
 * so files added while running (an automatic upscaler writing replace/auto)
 * apply from the next upload of their texture. A hand-made file wins over
 * replace/auto's file of the same name.
 *
 * Configured through ps3_host_set_texture_pack(); RSX_TEXPACK_DIR,
 * RSX_TEXPACK_DUMP=0/1 and RSX_TEXPACK_REPLACE=0/1 override it. Called on
 * the render thread only.
 */
#ifndef PS3RECOMP_RSX_TEXTURE_PACK_H
#define PS3RECOMP_RSX_TEXTURE_PACK_H

#include "ps3emu/ps3types.h"
#include "rsx_texture_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Replacements larger than this on either side are rejected. */
#define RSX_TEXPACK_MAX_DIM 8192u

typedef struct {
    u32 w, h, row_bytes;   /* row_bytes = w * channels, tightly packed */
    u8* data;
} rsx_texpack_level;

typedef struct {
    rsx_texfmt fmt;        /* RSX_TEXFMT_R8, _R8G8 or _R8G8B8A8          */
    u32 levels;            /* full chain down to 1x1                       */
    rsx_texpack_level level[RSX_MAX_TEXTURE_LEVELS];
} rsx_texpack_image;

/* Nonzero when dumping, or when at least one replacement was found. Cheap:
 * backends test it before anything else so a game without a pack pays
 * nothing per upload. Reads the configuration on its first call. */
int rsx_texpack_enabled(void);

/* Look up (and, in dump mode, dump) one guest texture's level 0.
 *
 *   where     the backend's address key for it (location/offset or EA);
 *             only used to stop dumping a texture rewritten every frame
 *   src       guest bytes of level 0
 *   rsx_fmt   SET_TEXTURE_FORMAT byte, LN/UN bits included
 *   pitch     SET_TEXTURE_CONTROL3 row pitch (0 = tightly packed)
 *
 * Returns 1 with `out` filled when a replacement exists and loaded; release
 * it with rsx_texpack_release(). Returns 0 otherwise, `out` untouched. */
int rsx_texpack_lookup(u64 where, const u8* src, u32 rsx_fmt, u32 w, u32 h,
                       u32 pitch, rsx_texpack_image* out);
void rsx_texpack_release(rsx_texpack_image* image);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_TEXTURE_PACK_H */

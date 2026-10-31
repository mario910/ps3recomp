/*
 * ps3recomp - RSX texture packs (see rsx_texture_pack.h)
 */
#include "rsx_texture_pack.h"
#include "ps3emu/host_controls.h"
/* A private, internal-linkage copy of stb: the runtime's lives in the codec
 * HLE (cellPngDec.c / cellPngEnc.c), which a standalone engine build such
 * as the engine unit test does not link. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../codec/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../codec/stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ---- configuration and tables -------------------------------------------- */

#define TP_MAX_DUMPS       16384u  /* files per run                         */
#define TP_ADDR_CHANGES    4u      /* contents dumped per address, at most  */
#define TP_SCAN_DEPTH      4       /* replace/ subfolder levels searched    */

typedef struct { u64 key; char* path; u32 count; } tp_entry;   /* key 0 = empty */
/* Replacement entries' count: where the file came from and whether it failed. */
#define TP_AUTO            1u      /* replace/auto, the automatic upscaler's */
#define TP_FAILED          2u      /* failed to load; not tried again        */
typedef struct { tp_entry* e; u32 cap, n; } tp_table;

static struct {
    int ready, dump, replace, replace_wanted;
    unsigned generation;     /* ps3_host_texture_pack_generation() scanned */
    char dir[1024];
    char auto_dir[1100];     /* "<dir>/replace<sep>auto<sep>" path prefix  */
    size_t auto_len;
    tp_table replacements;   /* id -> replace/ file           */
    tp_table dumped;         /* ids already in dump/ (no path) */
    tp_table addresses;      /* where -> count of dumps there  */
    u32 dumps;
} s_tp;

static tp_entry* tp_slot(tp_table* t, u64 key)
{
    if (!t->cap) return NULL;
    for (u32 i = (u32)(key ^ (key >> 32)) & (t->cap - 1);; i = (i + 1) & (t->cap - 1))
        if (t->e[i].key == key || !t->e[i].key) return &t->e[i];
}
static tp_entry* tp_find(tp_table* t, u64 key)
{
    tp_entry* e = tp_slot(t, key ? key : 1);
    return e && e->key ? e : NULL;
}
/* Returns the entry for `key`, inserting it (path NULL) when absent. */
static tp_entry* tp_insert(tp_table* t, u64 key)
{
    if (!key) key = 1;
    if ((t->n + 1) * 4 >= t->cap * 3) {
        tp_table grown = { 0, t->cap ? t->cap * 2 : 256, 0 };
        grown.e = (tp_entry*)calloc(grown.cap, sizeof *grown.e);
        if (!grown.e) return NULL;
        for (u32 i = 0; i < t->cap; i++)
            if (t->e[i].key) { *tp_slot(&grown, t->e[i].key) = t->e[i]; grown.n++; }
        free(t->e);
        *t = grown;
    }
    tp_entry* e = tp_slot(t, key);
    if (!e->key) { e->key = key; e->path = NULL; e->count = 0; t->n++; }
    return e;
}

/* ---- UTF-8 file system ---------------------------------------------------- */

#ifdef _WIN32
static int tp_wide(const char* s, wchar_t* out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}
static FILE* tp_fopen(const char* path, const wchar_t* mode)
{
    wchar_t w[1100];
    return tp_wide(path, w, 1100) ? _wfopen(w, mode) : NULL;
}
static void tp_mkdir(const char* path)
{
    wchar_t w[1100];
    if (tp_wide(path, w, 1100)) CreateDirectoryW(w, NULL);
}
#define TP_RB L"rb"
#define TP_WB L"wb"
#define TP_SEP "\\"
#else
static FILE* tp_fopen(const char* path, const char* mode) { return fopen(path, mode); }
static void tp_mkdir(const char* path) { mkdir(path, 0755); }
#define TP_RB "rb"
#define TP_WB "wb"
#define TP_SEP "/"
#endif

/* "<16 hex>_<w>x<h>_<2 hex>.png" -> id key; 0 when the name is not one. */
static u64 tp_name_key(const char* name)
{
    unsigned long long hash = 0;
    unsigned w = 0, h = 0, fmt = 0;
    int used = 0;
    if (strlen(name) < 26 ||
        sscanf(name, "%16llx_%ux%u_%2x.%n", &hash, &w, &h, &fmt, &used) != 4 || !used)
        return 0;
    const char* ext = name + used;
    if (!((ext[0] | 32) == 'p' && (ext[1] | 32) == 'n' && (ext[2] | 32) == 'g' && !ext[3]))
        return 0;
    /* The same fold as rsx_texpack_lookup(), so a name and a texture agree. */
    u64 key = (u64)hash ^ ((u64)w << 40) ^ ((u64)h << 20) ^ (u64)fmt;
    return key ? key : 1;
}

/* Calls back with every regular file under `dir` (UTF-8 path and name). */
static void tp_scan(const char* dir, int depth, void (*fn)(const char* path, const char* name, void* user),
                    void* user)
{
    char path[1100];
#ifdef _WIN32
    wchar_t pattern[1100];
    snprintf(path, sizeof path, "%s\\*", dir);
    if (!tp_wide(path, pattern, 1100)) return;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        char name[520];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL) ||
            name[0] == '.')
            continue;
        if (snprintf(path, sizeof path, "%s\\%s", dir, name) >= (int)sizeof path) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) tp_scan(path, depth - 1, fn, user);
        } else {
            fn(path, name, user);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, de->d_name) >= (int)sizeof path) continue;
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) tp_scan(path, depth - 1, fn, user);
        } else if (S_ISREG(st.st_mode)) {
            fn(path, de->d_name, user);
        }
    }
    closedir(d);
#endif
}

/* user is non-NULL on a rescan, which only adds what is new. */
static void tp_index_replacement(const char* path, const char* name, void* user)
{
    const u64 key = tp_name_key(name);
    if (!key) return;
    const u32 made = strncmp(path, s_tp.auto_dir, s_tp.auto_len) ? 0u : TP_AUTO;
    tp_entry* e = tp_insert(&s_tp.replacements, key);
    if (!e || (e->count & TP_FAILED)) return;
    if (e->path) {
        /* A hand-made file wins over replace/auto's upscale of the same
         * texture, whichever the scan meets first. */
        if ((e->count & TP_AUTO) && !made) {
            free(e->path);
            e->path = NULL;
        } else {
            if (!user && !made && !(e->count & TP_AUTO))
                fprintf(stderr, "[texpack] duplicate replacement %s ignored\n", path);
            return;
        }
    }
    const size_t n = strlen(path) + 1;
    e->path = (char*)malloc(n);
    if (e->path) memcpy(e->path, path, n);
    e->count = made;
}

static void tp_scan_replacements(int rescan)
{
    char sub[1100];
    snprintf(sub, sizeof sub, "%s/replace", s_tp.dir);
    tp_scan(sub, TP_SCAN_DEPTH, tp_index_replacement, rescan ? (void*)sub : NULL);
    s_tp.replace = s_tp.replacements.n != 0;
}
static void tp_index_dump(const char* path, const char* name, void* user)
{
    (void)path; (void)user;
    const u64 key = tp_name_key(name);
    if (key) tp_insert(&s_tp.dumped, key);
}

static void tp_init(void)
{
    s_tp.ready = 1;
    const char* dir = NULL;
    const unsigned flags = ps3_host_get_texture_pack(&dir);
    const char* env_dir = getenv("RSX_TEXPACK_DIR");
    const char* env_dump = getenv("RSX_TEXPACK_DUMP");
    const char* env_replace = getenv("RSX_TEXPACK_REPLACE");
    if (env_dir && *env_dir) dir = env_dir;
    s_tp.dump = env_dump ? atoi(env_dump) != 0 : (flags & PS3_HOST_TEXPACK_DUMP) != 0;
    s_tp.replace = env_replace ? atoi(env_replace) != 0 : (flags & PS3_HOST_TEXPACK_REPLACE) != 0;
    /* Environment-only use defaults to replacing, like the game's setting. */
    if (env_dir && *env_dir && !flags && !env_replace) s_tp.replace = 1;
    if (!dir || !*dir || strlen(dir) >= sizeof s_tp.dir - 16) { s_tp.dump = s_tp.replace = 0; return; }
    strcpy(s_tp.dir, dir);
    snprintf(s_tp.auto_dir, sizeof s_tp.auto_dir, "%s/replace" TP_SEP "auto" TP_SEP, s_tp.dir);
    s_tp.auto_len = strlen(s_tp.auto_dir);
    char sub[1100];
    s_tp.replace_wanted = s_tp.replace;
    s_tp.generation = ps3_host_texture_pack_generation();
    if (s_tp.replace) tp_scan_replacements(0);
    if (s_tp.dump) {
        tp_mkdir(s_tp.dir);
        snprintf(sub, sizeof sub, "%s/dump", s_tp.dir);
        tp_mkdir(sub);
        tp_scan(sub, 0, tp_index_dump, NULL);
    }
    fprintf(stderr, "[texpack] dir=%s replacements=%u dump=%d (%u already dumped)\n",
            s_tp.dir, s_tp.replacements.n, s_tp.dump, s_tp.dumped.n);
}

int rsx_texpack_enabled(void)
{
    if (!s_tp.ready) tp_init();
    /* Files added while running: the automatic upscaler's, or by hand. */
    if (s_tp.replace_wanted) {
        const unsigned generation = ps3_host_texture_pack_generation();
        if (generation != s_tp.generation) {
            s_tp.generation = generation;
            const u32 before = s_tp.replacements.n;
            tp_scan_replacements(1);
            if (s_tp.replacements.n != before)
                fprintf(stderr, "[texpack] %u new replacements\n", s_tp.replacements.n - before);
        }
    }
    return s_tp.dump || s_tp.replace;
}

/* ---- identity ------------------------------------------------------------- */

static u64 tp_mix(u64 h, u64 v)
{
    h ^= v * 0x9E3779B97F4A7C15ull;
    h = (h << 31) | (h >> 33);
    return h * 0xC2B2AE3D27D4EB4Full;
}
static u64 tp_bytes(u64 h, const u8* p, u32 n)
{
    u32 i = 0;
    for (; i + 8 <= n; i += 8) { u64 v; memcpy(&v, p + i, 8); h = tp_mix(h, v); }
    for (; i < n; i++) h = tp_mix(h, p[i]);
    return h;
}
static u64 tp_final(u64 h)
{
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull;
    return h ^ (h >> 33);
}
/* Hash of the level-0 texels the guest stored. A pitched linear image is
 * hashed row by row without its padding, which the game never wrote with
 * intent and which would otherwise split one image into many names. */
static u64 tp_content_hash(const u8* src, const rsx_tex_layout* tl, u32 w)
{
    u64 h = tl->swizzled ? 0x165667B19E3779F9ull : 0x27D4EB2F165667C5ull;
    const u32 packed = tl->bytes_per_texel * w;
    if (!tl->compressed && !tl->swizzled && packed && tl->row_bytes > packed) {
        for (u32 y = 0; y < tl->rows; y++) h = tp_bytes(h, src + (size_t)y * tl->row_bytes, packed);
    } else {
        h = tp_bytes(h, src, tl->face_bytes);
    }
    return tp_final(h);
}

/* ---- decoding for the dump ------------------------------------------------ */

static void tp_565(u16 c, u8 out[4])
{
    const u32 r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    out[0] = (u8)((r << 3) | (r >> 2));
    out[1] = (u8)((g << 2) | (g >> 4));
    out[2] = (u8)((b << 3) | (b >> 2));
    out[3] = 255;
}
/* One BC1/2/3 block (little-endian, as RSX stores DXT) into a 4x4 RGBA tile. */
static void tp_bc_block(const u8* b, rsx_texfmt fmt, u8 tile[16][4])
{
    const u8* c = fmt == RSX_TEXFMT_BC1 ? b : b + 8;
    const u16 c0 = (u16)(c[0] | (c[1] << 8)), c1 = (u16)(c[2] | (c[3] << 8));
    u8 pal[4][4];
    tp_565(c0, pal[0]);
    tp_565(c1, pal[1]);
    const int four = fmt != RSX_TEXFMT_BC1 || c0 > c1;
    for (int k = 0; k < 3; k++) {
        if (four) {
            pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        } else {
            pal[2][k] = (u8)((pal[0][k] + pal[1][k] + 1) / 2);
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = four ? 255 : 0;
    const u32 bits = (u32)c[4] | ((u32)c[5] << 8) | ((u32)c[6] << 16) | ((u32)c[7] << 24);
    for (int i = 0; i < 16; i++) memcpy(tile[i], pal[(bits >> (2 * i)) & 3u], 4);
    if (fmt == RSX_TEXFMT_BC2) {
        for (int i = 0; i < 16; i++) {
            const u32 a = (b[i / 2] >> ((i & 1) * 4)) & 15u;
            tile[i][3] = (u8)(a * 17u);
        }
    } else if (fmt == RSX_TEXFMT_BC3) {
        u8 a[8];
        a[0] = b[0]; a[1] = b[1];
        if (a[0] > a[1]) {
            for (int k = 1; k < 7; k++) a[k + 1] = (u8)(((7 - k) * a[0] + k * a[1] + 3) / 7);
        } else {
            for (int k = 1; k < 5; k++) a[k + 1] = (u8)(((5 - k) * a[0] + k * a[1] + 2) / 5);
            a[6] = 0; a[7] = 255;
        }
        u64 idx = 0;
        for (int k = 0; k < 6; k++) idx |= (u64)b[2 + k] << (8 * k);
        for (int i = 0; i < 16; i++) tile[i][3] = a[(idx >> (3 * i)) & 7u];
    }
}

/* Colour art only: B8, the ARGB family and the 16-bit packed colours, G8B8
 * and DXT. HILO normal maps, depth and float formats are data, not images. */
static int tp_eligible(u32 base)
{
    switch (base) {
    case 0x81: case 0x85: case 0x9E: case 0x8B:
    case 0x82: case 0x83: case 0x84: case 0x97: case 0x9D:
    case 0x86: case 0x87: case 0x88: return 1;
    default:                         return 0;
    }
}

static int tp_channels(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8:       return 1;
    case RSX_TEXFMT_R8G8:     return 2;
    case RSX_TEXFMT_R8G8B8A8:
    case RSX_TEXFMT_BC1:
    case RSX_TEXFMT_BC2:
    case RSX_TEXFMT_BC3:      return 4;
    default:                  return 0;
    }
}

typedef struct { FILE* f; int ok; } tp_writer;
static void tp_write(void* ctx, void* data, int size)
{
    tp_writer* w = (tp_writer*)ctx;
    if (w->ok && fwrite(data, 1, (size_t)size, w->f) != (size_t)size) w->ok = 0;
}

static void tp_dump(u64 key, const char* name, const u8* src, const rsx_tex_layout* tl,
                    u32 w, u32 h)
{
    const int n = tp_channels(tl->fmt);
    u8* decoded = (u8*)malloc((size_t)tl->dst_row_bytes * tl->rows);
    u8* rgba = (u8*)malloc((size_t)w * h * (u32)n);
    if (!decoded || !rgba) { free(decoded); free(rgba); return; }
    rsx_texture_decode(decoded, tl->dst_row_bytes, src, w, h, tl, rsx_texture_argb_is_rgba());
    if (tl->compressed) {
        const u32 bw = (w + 3) / 4;
        for (u32 by = 0; by < tl->rows; by++)
            for (u32 bx = 0; bx < bw; bx++) {
                u8 tile[16][4];
                tp_bc_block(decoded + (size_t)by * tl->dst_row_bytes + (size_t)bx * tl->block_bytes,
                            tl->fmt, tile);
                for (u32 i = 0; i < 16; i++) {
                    const u32 x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < w && y < h) memcpy(rgba + ((size_t)y * w + x) * 4, tile[i], 4);
                }
            }
    } else {
        for (u32 y = 0; y < h; y++)
            memcpy(rgba + (size_t)y * w * (u32)n, decoded + (size_t)y * tl->dst_row_bytes, (size_t)w * (u32)n);
    }
    free(decoded);
    char path[1200];
    snprintf(path, sizeof path, "%s/dump/%s", s_tp.dir, name);
    tp_writer out = { tp_fopen(path, TP_WB), 1 };
    if (out.f) {
        out.ok = stbi_write_png_to_func(tp_write, &out, (int)w, (int)h, n, rgba, (int)(w * (u32)n)) && out.ok;
        fclose(out.f);
    }
    free(rgba);
    if (!out.f || !out.ok) {
        fprintf(stderr, "[texpack] cannot write %s\n", path);
        return;
    }
    tp_insert(&s_tp.dumped, key);
    s_tp.dumps++;
}

/* ---- replacement ---------------------------------------------------------- */

void rsx_texpack_release(rsx_texpack_image* image)
{
    if (!image) return;
    for (u32 m = 0; m < image->levels; m++) free(image->level[m].data);
    memset(image, 0, sizeof *image);
}

/* Box-filter the chain below level 0. */
static int tp_mips(rsx_texpack_image* im, int n)
{
    while (im->levels < RSX_MAX_TEXTURE_LEVELS) {
        const rsx_texpack_level* a = &im->level[im->levels - 1];
        if (a->w == 1 && a->h == 1) break;
        rsx_texpack_level* b = &im->level[im->levels];
        b->w = a->w > 1 ? a->w / 2 : 1;
        b->h = a->h > 1 ? a->h / 2 : 1;
        b->row_bytes = b->w * (u32)n;
        b->data = (u8*)malloc((size_t)b->row_bytes * b->h);
        if (!b->data) return 0;
        for (u32 y = 0; y < b->h; y++) {
            const u32 y0 = y * 2 < a->h ? y * 2 : a->h - 1, y1 = y * 2 + 1 < a->h ? y * 2 + 1 : y0;
            for (u32 x = 0; x < b->w; x++) {
                const u32 x0 = x * 2 < a->w ? x * 2 : a->w - 1, x1 = x * 2 + 1 < a->w ? x * 2 + 1 : x0;
                for (int c = 0; c < n; c++) {
                    const u32 s = a->data[(size_t)y0 * a->row_bytes + x0 * (u32)n + c] +
                                  a->data[(size_t)y0 * a->row_bytes + x1 * (u32)n + c] +
                                  a->data[(size_t)y1 * a->row_bytes + x0 * (u32)n + c] +
                                  a->data[(size_t)y1 * a->row_bytes + x1 * (u32)n + c];
                    b->data[(size_t)y * b->row_bytes + x * (u32)n + c] = (u8)((s + 2) / 4);
                }
            }
        }
        im->levels++;
    }
    return 1;
}

static int tp_load(const char* path, rsx_texfmt fmt, u32 gw, u32 gh, rsx_texpack_image* out)
{
    FILE* f = tp_fopen(path, TP_RB);
    if (!f) return 0;
    u8* file = NULL;
    long size = 0;
    if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) > 0 && size < (256l << 20) && !fseek(f, 0, SEEK_SET)) {
        file = (u8*)malloc((size_t)size);
        if (file && fread(file, 1, (size_t)size, f) != (size_t)size) { free(file); file = NULL; }
    }
    fclose(f);
    if (!file) return 0;
    const int n = tp_channels(fmt);
    int w = 0, h = 0, comp = 0;
    u8* pixels = stbi_load_from_memory(file, (int)size, &w, &h, &comp, n);
    free(file);
    if (!pixels) {
        fprintf(stderr, "[texpack] cannot decode %s\n", path);
        return 0;
    }
    if (w < 1 || h < 1 || (u32)w > RSX_TEXPACK_MAX_DIM || (u32)h > RSX_TEXPACK_MAX_DIM) {
        fprintf(stderr, "[texpack] %s is %dx%d, above the %u limit\n", path, w, h, RSX_TEXPACK_MAX_DIM);
        stbi_image_free(pixels);
        return 0;
    }
    /* A different shape still samples, stretched; most likely a crop by
     * mistake, so say so. */
    if ((u64)w * gh != (u64)h * gw &&
        ((u64)w * gh * 100 < (u64)h * gw * 99 || (u64)w * gh * 99 > (u64)h * gw * 100))
        fprintf(stderr, "[texpack] %s is %dx%d, not the %ux%u texture's aspect ratio\n", path, w, h, gw, gh);
    rsx_texpack_image im;
    memset(&im, 0, sizeof im);
    im.fmt = n == 1 ? RSX_TEXFMT_R8 : n == 2 ? RSX_TEXFMT_R8G8 : RSX_TEXFMT_R8G8B8A8;
    im.levels = 1;
    im.level[0].w = (u32)w;
    im.level[0].h = (u32)h;
    im.level[0].row_bytes = (u32)w * (u32)n;
    /* stb's buffer is malloc'd unless STBI_MALLOC says otherwise; copy so
     * release can free() every level the same way. */
    im.level[0].data = (u8*)malloc((size_t)w * h * (u32)n);
    if (im.level[0].data) memcpy(im.level[0].data, pixels, (size_t)w * h * (u32)n);
    stbi_image_free(pixels);
    if (!im.level[0].data || !tp_mips(&im, n)) {
        rsx_texpack_release(&im);
        return 0;
    }
    *out = im;
    return 1;
}

int rsx_texpack_lookup(u64 where, const u8* src, u32 rsx_fmt, u32 w, u32 h, u32 pitch,
                       rsx_texpack_image* out)
{
    if (!rsx_texpack_enabled() || !src || !w || !h || !out) return 0;
    rsx_tex_layout tl;
    rsx_texture_layout_pitched(rsx_fmt, w, h, pitch, &tl);
    if (!tp_eligible(tl.base_format) || !tp_channels(tl.fmt) || !tl.face_bytes) return 0;
    const u32 fmt = tl.base_format & 0xFFu;
    const u64 hash = tp_content_hash(src, &tl, w);
    u64 key = hash ^ ((u64)w << 40) ^ ((u64)h << 20) ^ (u64)fmt;
    if (!key) key = 1;

    if (s_tp.replace) {
        tp_entry* e = tp_find(&s_tp.replacements, key);
        if (e && e->path) {
            if (tp_load(e->path, tl.fmt, w, h, out)) return 1;
            fprintf(stderr, "[texpack] %s failed to load; using the game's texture\n", e->path);
            free(e->path);
            e->path = NULL;   /* once is enough */
            e->count |= TP_FAILED;
        }
    }

    if (s_tp.dump && s_tp.dumps < TP_MAX_DUMPS && w * h >= 64 && !tp_find(&s_tp.dumped, key)) {
        /* An address rewritten with new contents again and again is a video
         * plane or a rendered surface, not art: a few dumps, then no more. */
        tp_entry* a = tp_insert(&s_tp.addresses,
                                tp_final(where ^ ((u64)w << 48) ^ ((u64)h << 32) ^ ((u64)fmt << 24)));
        if (a && a->count < TP_ADDR_CHANGES) {
            a->count++;
            char name[64];
            snprintf(name, sizeof name, "%016llx_%ux%u_%02x.png", (unsigned long long)hash, w, h, fmt);
            tp_dump(key, name, src, &tl, w, h);
            if (s_tp.dumps == TP_MAX_DUMPS)
                fprintf(stderr, "[texpack] dump limit of %u textures reached\n", TP_MAX_DUMPS);
        }
    }
    return 0;
}

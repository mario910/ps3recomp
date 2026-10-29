/* Persistent HLSL -> bytecode cache for the guest shaders translated at draw
 * time. A scene change met with dozens of new programs compiles each one
 * synchronously on the RSX thread (~10-20 ms apiece), which shows up as a
 * multi-hundred-millisecond frame. The same source always compiles to the same
 * bytecode, so a file keyed by (target, entry, flags, source) replays it.
 *
 * Enabled only when RSX_SHADER_CACHE_DIR names a directory. Each file stores
 * the complete source and is compared byte for byte before use: a hash
 * collision or a stale file can only cost a recompile, never a wrong shader. */
#ifndef RSX_SHADER_DISK_CACHE_H
#define RSX_SHADER_DISK_CACHE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* rsx_shader_cache_dir(void)
{
    static int init = 0;
    static char dir[512];
    if (!init) {
        const char* e = getenv("RSX_SHADER_CACHE_DIR");
        if (e && *e && strlen(e) < sizeof dir - 40) {
            strcpy(dir, e);
            CreateDirectoryA(dir, NULL);
        } else dir[0] = 0;
        init = 1;
    }
    return dir[0] ? dir : NULL;
}

/* Time spent in D3DCompile on cache misses since the backend last reset
 * these (once per presented frame): what a [slow-frame] report charges to
 * shader compilation. Single translation unit (the D3D12 backend). */
static double s_rsx_stall_hlsl_ms;
static unsigned s_rsx_stall_hlsl_n;
static double s_rsx_stall_disk_ms;   /* cache hits: file open + read */
static unsigned s_rsx_stall_disk_n;

static uint64_t rsx_shader_fnv(uint64_t h, const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

typedef struct { char magic[8]; uint32_t target_len, entry_len, flags, src_len, code_len; } RsxShaderCacheHeader;

static HRESULT rsx_d3d_compile_cached(const char* src, size_t src_len, const char* name,
                                      const char* entry, const char* target, UINT flags,
                                      ID3DBlob** code, ID3DBlob** errors)
{
    const char* dir = rsx_shader_cache_dir();
    if (!dir)
        return D3DCompile(src, src_len, name, NULL, NULL, entry, target, flags, 0, code, errors);
    const uint32_t tl = (uint32_t)strlen(target), el = (uint32_t)strlen(entry);
    uint64_t h = 14695981039346656037ull;
    h = rsx_shader_fnv(h, target, tl + 1);
    h = rsx_shader_fnv(h, entry, el + 1);
    h = rsx_shader_fnv(h, &flags, sizeof flags);
    h = rsx_shader_fnv(h, src, src_len);
    char path[600];
    snprintf(path, sizeof path, "%s\\%016llx.dxbc", dir, (unsigned long long)h);

    LARGE_INTEGER disk_t0; QueryPerformanceCounter(&disk_t0);
    FILE* f = fopen(path, "rb");
    if (f) {
        RsxShaderCacheHeader hd;
        int ok = fread(&hd, sizeof hd, 1, f) == 1 && !memcmp(hd.magic, "RSXSHC1", 8) &&
                 hd.target_len == tl && hd.entry_len == el && hd.flags == flags &&
                 hd.src_len == (uint32_t)src_len && hd.code_len && hd.code_len < (64u << 20);
        char* key = NULL;
        if (ok) {
            const size_t key_len = (size_t)tl + el + src_len;
            key = (char*)malloc(key_len ? key_len : 1);
            ok = key && fread(key, 1, key_len, f) == key_len &&
                 !memcmp(key, target, tl) && !memcmp(key + tl, entry, el) &&
                 !memcmp(key + tl + el, src, src_len);
        }
        free(key);
        ID3DBlob* blob = NULL;
        if (ok && SUCCEEDED(D3DCreateBlob(hd.code_len, &blob)) &&
            fread(blob->lpVtbl->GetBufferPointer(blob), 1, hd.code_len, f) == hd.code_len) {
            fclose(f);
            *code = blob;
            if (errors) *errors = NULL;
            LARGE_INTEGER disk_t1, disk_f;
            QueryPerformanceCounter(&disk_t1); QueryPerformanceFrequency(&disk_f);
            s_rsx_stall_disk_ms += (double)(disk_t1.QuadPart - disk_t0.QuadPart) * 1000.0 / (double)disk_f.QuadPart;
            s_rsx_stall_disk_n++;
            return S_OK;
        }
        if (blob) blob->lpVtbl->Release(blob);
        fclose(f);
    }

    LARGE_INTEGER compile_t0, compile_t1, compile_f;
    QueryPerformanceCounter(&compile_t0);
    HRESULT hr = D3DCompile(src, src_len, name, NULL, NULL, entry, target, flags, 0, code, errors);
    QueryPerformanceCounter(&compile_t1); QueryPerformanceFrequency(&compile_f);
    s_rsx_stall_hlsl_ms += (double)(compile_t1.QuadPart - compile_t0.QuadPart) * 1000.0 / (double)compile_f.QuadPart;
    s_rsx_stall_hlsl_n++;
    if (SUCCEEDED(hr) && *code) {
        char tmp[620];
        snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentThreadId());
        FILE* o = fopen(tmp, "wb");
        if (o) {
            RsxShaderCacheHeader hd;
            memcpy(hd.magic, "RSXSHC1", 8);
            hd.target_len = tl; hd.entry_len = el; hd.flags = flags;
            hd.src_len = (uint32_t)src_len;
            hd.code_len = (uint32_t)(*code)->lpVtbl->GetBufferSize(*code);
            int ok = fwrite(&hd, sizeof hd, 1, o) == 1 && fwrite(target, 1, tl, o) == tl &&
                     fwrite(entry, 1, el, o) == el && fwrite(src, 1, src_len, o) == src_len &&
                     fwrite((*code)->lpVtbl->GetBufferPointer(*code), 1, hd.code_len, o) == hd.code_len;
            ok = fclose(o) == 0 && ok;
            if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
        }
    }
    return hr;
}

#endif

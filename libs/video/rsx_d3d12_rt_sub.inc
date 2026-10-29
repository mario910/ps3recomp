/* A linear texture at a render target's address that is SMALLER than the
 * target reads only the target's top-left w x h texels: same memory, same
 * pitch, texture origin = surface origin. Binding the whole target resource
 * instead stretched UV 0..1 over all of it, so content rendered into a corner
 * (Buzz Quiz World's 100x100 avatar head in a 400x200 surface, the 256x64
 * podium name in a 1024x256 one) showed at a fraction of its size. Bind a
 * texture-sized copy of that region, refreshed on the GPU only after the
 * target was drawn or cleared again. */
static ID3D12Resource* rt_sub_source(int index, D3D12DrawRecord* dr, unsigned unit)
{
    OffRT* r = &s_d3d.off_rt[index];
    const u32 w = dr->tex[unit].w, h = dr->tex[unit].h;
    if (!(dr->tex[unit].fmt & 0x20) || r->sx * r->sy != 1 || !w || !h ||
        w > r->w || h > r->h || (w == r->w && h == r->h))
        return NULL;
    if (r->sub && (r->sub_w != w || r->sub_h != h)) {
        r->sub->lpVtbl->Release(r->sub);   /* as off_rt slot reuse does */
        r->sub = NULL;
    }
    if (!r->sub) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = td.MipLevels = td.SampleDesc.Count = 1;
        td.Format = (DXGI_FORMAT)r->dxgi;
        HRESULT hr = s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
            &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&r->sub);
        if (FAILED(hr)) { r->sub = NULL; return NULL; }
        r->sub_w = w; r->sub_h = h;
        r->sub_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        r->sub_dirty = 1;
        static int shown;
        if (PS3_LOG_FIRST(shown, 16))
            fprintf(stderr, "[rt-sub] off=%08X target=%ux%u sampled as %ux%u: binding that region\n",
                    r->off, r->w, r->h, w, h);
    }
    dr->tex_rt_sub |= 1u << unit;
    return r->sub;
}

/* Before a draw that samples it: copy the target's region if it changed. */
static int rt_sub_copy(int index)
{
    if (index < 0) return 0;
    OffRT* r = &s_d3d.off_rt[index];
    if (!r->sub || !r->sub_dirty) return 0;
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 0, NULL, FALSE, NULL);
    off_rt_transition(index, D3D12_RESOURCE_STATE_COPY_SOURCE);
    zeta_barrier(r->sub, &r->sub_state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = r->sub; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = r->res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = {0, 0, 0, r->sub_w, r->sub_h, 1};
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, &box);
    zeta_barrier(r->sub, &r->sub_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    off_rt_transition(index, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    r->sub_dirty = 0;
    return 1;
}

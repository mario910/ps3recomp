/* Included after s_d3d, wait_for_gpu and the off-RT helpers.
 *
 * Display path. The guest's frame is drawn into render_targets[] -- plain
 * offscreen textures at the internal render size (guest size x render
 * scale) -- and only presentation touches the swapchain. The swapchain
 * follows the window's client area (window size preference, borderless
 * desktop resolution, user resizing, DPI), and one draw scales the frame
 * into it, keeping the guest's aspect ratio with black bars. The host
 * overlay draws last, on the swapchain image at window resolution.
 *
 * Window size and internal size are independent: a 4K window can show a
 * 720p frame and a 1080p window a 1440p one. */

/* Full-screen triangle; uv (0,0) at the top left. */
static const char s_present_vs[] =
    "struct V{float4 p:SV_POSITION;float2 uv:TEXCOORD0;};\n"
    "V main(uint id:SV_VertexID){V o;float2 t=float2((id<<1)&2,id&2);\n"
    "o.uv=t;o.p=float4(t*float2(2,-2)+float2(-1,1),0,1);return o;}\n";
static const char s_present_ps[] =
    "Texture2D frame:register(t0);SamplerState smp:register(s0);\n"
    "struct V{float4 p:SV_POSITION;float2 uv:TEXCOORD0;};\n"
    "float4 main(V i):SV_TARGET{return float4(frame.Sample(smp,i.uv).rgb,1);}\n";

static D3D12_CPU_DESCRIPTOR_HANDLE present_swap_rtv(u32 index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.swap_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.swap_rtv_heap, &h);
    h.ptr += (SIZE_T)index * s_d3d.rtv_descriptor_size;
    return h;
}

static void present_release_buffers(void)
{
    for (u32 i = 0; i < FRAME_COUNT; i++)
        if (s_d3d.swap_buffers[i]) {
            s_d3d.swap_buffers[i]->lpVtbl->Release(s_d3d.swap_buffers[i]);
            s_d3d.swap_buffers[i] = NULL;
        }
}

static int present_acquire_buffers(void)
{
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (FAILED(s_d3d.swap_chain->lpVtbl->GetBuffer(s_d3d.swap_chain, i, &IID_ID3D12Resource,
                                                       (void**)&s_d3d.swap_buffers[i]))) {
            present_release_buffers();
            return -1;
        }
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_d3d.swap_buffers[i], NULL,
                                                     present_swap_rtv(i));
    }
    return 0;
}

/* A full-screen-triangle pipeline writing R8G8B8A8 with no blend or depth. */
static HRESULT present_pipeline(ID3D12RootSignature* root, const char* vs_src, size_t vs_len,
                                const char* ps_src, size_t ps_len, ID3D12PipelineState** out)
{
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL;
    HRESULT hr = D3DCompile(vs_src, vs_len, "present_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vs, &err);
    if (err) { err->lpVtbl->Release(err); err = NULL; }
    if (SUCCEEDED(hr)) hr = D3DCompile(ps_src, ps_len, "present_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &ps, &err);
    if (err) {
        if (FAILED(hr)) fprintf(stderr, "[D3D12] present shader: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err));
        err->lpVtbl->Release(err);
    }
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = root;
        pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
        pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
        pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
        pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd,
                &IID_ID3D12PipelineState, (void**)out);
    }
    if (vs) vs->lpVtbl->Release(vs);
    if (ps) ps->lpVtbl->Release(ps);
    return hr;
}

/* FSR 1: one SRV table (t0) and eight root constants (b0), no sampler. */
static void fsr_init(void)
{
    D3D12_DESCRIPTOR_RANGE range = {0};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2] = {0};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 8;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {0};
    rs.NumParameters = 2; rs.pParameters = params;
    ID3DBlob *sig = NULL, *err = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (SUCCEEDED(hr))
        hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                &IID_ID3D12RootSignature, (void**)&s_d3d.fsr_root);
    if (sig) sig->lpVtbl->Release(sig);
    if (err) err->lpVtbl->Release(err);
    if (SUCCEEDED(hr)) hr = present_pipeline(s_d3d.fsr_root, rsx_fsr1_vs, sizeof rsx_fsr1_vs - 1,
                                             rsx_fsr1_easu_ps, sizeof rsx_fsr1_easu_ps - 1, &s_d3d.fsr_easu);
    if (SUCCEEDED(hr)) hr = present_pipeline(s_d3d.fsr_root, rsx_fsr1_vs, sizeof rsx_fsr1_vs - 1,
                                             rsx_fsr1_rcas_ps, sizeof rsx_fsr1_rcas_ps - 1, &s_d3d.fsr_rcas);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D12] FSR 1 unavailable (0x%08lX); bilinear presentation\n", hr);
        if (s_d3d.fsr_easu) { s_d3d.fsr_easu->lpVtbl->Release(s_d3d.fsr_easu); s_d3d.fsr_easu = NULL; }
        if (s_d3d.fsr_rcas) { s_d3d.fsr_rcas->lpVtbl->Release(s_d3d.fsr_rcas); s_d3d.fsr_rcas = NULL; }
    }
}

/* EASU's output, the size of the letterboxed output rectangle; recreated
 * when that changes. Kept in PIXEL_SHADER_RESOURCE between frames. */
static int fsr_mid_ensure(u32 w, u32 h)
{
    if (s_d3d.fsr_mid && s_d3d.fsr_mid_w == w && s_d3d.fsr_mid_h == h) return 1;
    if (s_d3d.fsr_mid) {
        wait_for_gpu();
        s_d3d.fsr_mid->lpVtbl->Release(s_d3d.fsr_mid);
        s_d3d.fsr_mid = NULL;
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&s_d3d.fsr_mid))) {
        s_d3d.fsr_mid = NULL;
        return 0;
    }
    s_d3d.fsr_mid_w = w; s_d3d.fsr_mid_h = h;
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_d3d.fsr_mid, NULL, present_swap_rtv(FRAME_COUNT));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
    h.ptr += (SIZE_T)FSR_SRV_SLOT * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.fsr_mid, &sv, h);
    return 1;
}

/* Root signature, pipeline and one SRV per offscreen frame. Failure leaves
 * present_pso NULL: the frame is then copied 1:1 into the top-left corner,
 * so a broken scaler never costs the picture. */
static void present_init(void)
{
    D3D12_DESCRIPTOR_RANGE range = {0};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER param = {0};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp = {0};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {0};
    rs.NumParameters = 1; rs.pParameters = &param;
    rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    ID3DBlob *sig = NULL, *err = NULL, *vs = NULL, *ps = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (SUCCEEDED(hr))
        hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                &IID_ID3D12RootSignature, (void**)&s_d3d.present_root);
    if (sig) sig->lpVtbl->Release(sig);
    if (err) { err->lpVtbl->Release(err); err = NULL; }
    if (SUCCEEDED(hr)) hr = D3DCompile(s_present_vs, sizeof s_present_vs - 1, "present_vs", NULL, NULL,
                                       "main", "vs_5_0", 0, 0, &vs, &err);
    if (err) { err->lpVtbl->Release(err); err = NULL; }
    if (SUCCEEDED(hr)) hr = D3DCompile(s_present_ps, sizeof s_present_ps - 1, "present_ps", NULL, NULL,
                                       "main", "ps_5_0", 0, 0, &ps, &err);
    if (err) { err->lpVtbl->Release(err); err = NULL; }
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = s_d3d.present_root;
        pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
        pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
        pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
        pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd,
                &IID_ID3D12PipelineState, (void**)&s_d3d.present_pso);
    }
    if (vs) vs->lpVtbl->Release(vs);
    if (ps) ps->lpVtbl->Release(ps);
    if (SUCCEEDED(hr)) fsr_init();
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D12] present scaler unavailable (0x%08lX); unscaled copy\n", hr);
        if (s_d3d.present_pso) { s_d3d.present_pso->lpVtbl->Release(s_d3d.present_pso); s_d3d.present_pso = NULL; }
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
        h.ptr += (SIZE_T)(PRESENT_SRV_BASE + i) * s_d3d.srv_inc;
        s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.render_targets[i], &sv, h);
    }
}

/* Follow the client area. ResizeBuffers needs every reference to the old
 * buffers gone, GPU work included; the command list being recorded has not
 * touched them yet (the present pass is their only user). A failure keeps
 * the old size and tries again on a later frame. */
static void present_resize(u32 cw, u32 ch)
{
    if (cw == s_d3d.swap_w && ch == s_d3d.swap_h) return;
    wait_for_gpu();
    present_release_buffers();
    HRESULT hr = s_d3d.swap_chain->lpVtbl->ResizeBuffers(s_d3d.swap_chain, FRAME_COUNT, cw, ch,
                                                         DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (SUCCEEDED(hr)) { s_d3d.swap_w = cw; s_d3d.swap_h = ch; }
    else fprintf(stderr, "[D3D12] ResizeBuffers %ux%u failed (0x%08lX); keeping %ux%u\n",
                 cw, ch, hr, s_d3d.swap_w, s_d3d.swap_h);
    if (present_acquire_buffers())
        fprintf(stderr, "[D3D12] swapchain buffers unavailable after resize\n");
    else if (SUCCEEDED(hr))
        fprintf(stderr, "[D3D12] swapchain %ux%u (internal %ux%u)\n", cw, ch, s_d3d.width, s_d3d.height);
}

static void present_barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
}

/* render_targets[fi] is in RENDER_TARGET state on entry and on exit. A
 * minimized window skips the pass; Present still runs (and is occluded),
 * so frame pacing and the guest are unaffected. */
static void present_pass(u32 fi)
{
    unsigned cw, ch;
    if (!rsx_host_window_client(&s_host_window, &cw, &ch)) return;
    present_resize(cw, ch);
    const u32 bi = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);
    ID3D12Resource* swap = bi < FRAME_COUNT ? s_d3d.swap_buffers[bi] : NULL;
    if (!swap) return;
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = present_swap_rtv(bi);
    const ps3_host_rect fit = ps3_host_fit_rect(s_d3d.swap_w, s_d3d.swap_h, s_d3d.guest_w, s_d3d.guest_h);
    unsigned filter, sharpness;
    ps3_host_get_upscaler(&filter, &sharpness);
    /* FSR 1 is an upscaler: used only when the output is larger than the
     * frame, never for downsampling a high internal resolution. */
    const int fsr = filter == PS3_HOST_UPSCALE_FSR1 && s_d3d.fsr_rcas &&
                    (fit.width > s_d3d.width || fit.height > s_d3d.height) &&
                    fsr_mid_ensure(fit.width, fit.height);
    if (fsr) {
        D3D12_GPU_DESCRIPTOR_HANDLE srv;
        s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &srv);
        const UINT64 frame_srv = srv.ptr + (UINT64)(PRESENT_SRV_BASE + fi) * s_d3d.srv_inc;
        const UINT64 mid_srv = srv.ptr + (UINT64)FSR_SRV_SLOT * s_d3d.srv_inc;
        float constants[8] = {(float)s_d3d.width, (float)s_d3d.height, (float)fit.width, (float)fit.height,
                              rsx_fsr1_sharpness(sharpness), 0.0f, 0.0f, 0.0f};
        /* EASU: frame -> fsr_mid. */
        present_barrier(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        present_barrier(s_d3d.fsr_mid, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE mid_rtv = present_swap_rtv(FRAME_COUNT);
        cl->lpVtbl->OMSetRenderTargets(cl, 1, &mid_rtv, FALSE, NULL);
        const D3D12_VIEWPORT mid_vp = {0.0f, 0.0f, (float)fit.width, (float)fit.height, 0.0f, 1.0f};
        const D3D12_RECT mid_sc = {0, 0, (LONG)fit.width, (LONG)fit.height};
        cl->lpVtbl->RSSetViewports(cl, 1, &mid_vp);
        cl->lpVtbl->RSSetScissorRects(cl, 1, &mid_sc);
        cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.fsr_root);
        cl->lpVtbl->SetPipelineState(cl, s_d3d.fsr_easu);
        cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_d3d.srv_heap);
        D3D12_GPU_DESCRIPTOR_HANDLE table = {frame_srv};
        cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, table);
        cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 8, constants, 0);
        cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
        present_barrier(s_d3d.fsr_mid, D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        /* RCAS: fsr_mid -> the output rectangle of the swapchain image. */
        present_barrier(swap, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
        if (fit.width != s_d3d.swap_w || fit.height != s_d3d.swap_h) {
            static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            cl->lpVtbl->ClearRenderTargetView(cl, rtv, black, 0, NULL);
        }
        const D3D12_VIEWPORT vp = {(float)fit.x, (float)fit.y, (float)fit.width, (float)fit.height, 0.0f, 1.0f};
        const D3D12_RECT sc = {fit.x, fit.y, fit.x + (LONG)fit.width, fit.y + (LONG)fit.height};
        cl->lpVtbl->RSSetViewports(cl, 1, &vp);
        cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
        cl->lpVtbl->SetPipelineState(cl, s_d3d.fsr_rcas);
        table.ptr = mid_srv;
        cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, table);
        constants[6] = (float)fit.x; constants[7] = (float)fit.y;
        cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 8, constants, 0);
        cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    } else if (s_d3d.present_pso) {
        present_barrier(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        present_barrier(swap, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
        if (fit.width != s_d3d.swap_w || fit.height != s_d3d.swap_h) {
            static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            cl->lpVtbl->ClearRenderTargetView(cl, rtv, black, 0, NULL);
        }
        const D3D12_VIEWPORT vp = {(float)fit.x, (float)fit.y, (float)fit.width, (float)fit.height, 0.0f, 1.0f};
        const D3D12_RECT sc = {fit.x, fit.y, fit.x + (LONG)fit.width, fit.y + (LONG)fit.height};
        cl->lpVtbl->RSSetViewports(cl, 1, &vp);
        cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
        cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.present_root);
        cl->lpVtbl->SetPipelineState(cl, s_d3d.present_pso);
        cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_d3d.srv_heap);
        D3D12_GPU_DESCRIPTOR_HANDLE srv;
        s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &srv);
        srv.ptr += (UINT64)(PRESENT_SRV_BASE + fi) * s_d3d.srv_inc;
        cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, srv);
        cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    } else {
        /* No scaler: an unscaled copy, as large as both images allow. */
        present_barrier(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        present_barrier(swap, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = swap; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = s_d3d.render_targets[fi]; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box = {0, 0, 0, s_d3d.width < s_d3d.swap_w ? s_d3d.width : s_d3d.swap_w,
                         s_d3d.height < s_d3d.swap_h ? s_d3d.height : s_d3d.swap_h, 1};
        cl->lpVtbl->CopyTextureRegion(cl, &dst, 0, 0, 0, &src, &box);
        present_barrier(swap, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        present_barrier(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    }
    if (ps3_host_overlay_enabled()) {
        /* The menu draws at window resolution, over the bars too. */
        const ps3_host_overlay_frame frame = {s_d3d.hwnd, s_d3d.device, s_d3d.cmd_queue, cl, rtv.ptr,
            s_d3d.swap_w, s_d3d.swap_h, FRAME_COUNT, DXGI_FORMAT_R8G8B8A8_UNORM};
        void* dpi = rsx_host_window_dpi_enter();
        ps3_host_overlay_draw(&frame);
        rsx_host_window_dpi_leave(dpi);
    }
    present_barrier(swap, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    present_barrier(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
}

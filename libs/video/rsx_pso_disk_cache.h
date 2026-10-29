/* Persistent D3D12 pipeline library for the pipelines built from translated
 * guest shaders. With the bytecode already cached (rsx_shader_disk_cache.h) a
 * new pipeline still costs ~7 ms of driver compilation on the RSX thread; an
 * ID3D12PipelineLibrary saved beside the shader cache replays it.
 *
 * Enabled only when RSX_SHADER_CACHE_DIR is set. A pipeline is looked up by a
 * hash of its full description, and the runtime itself rejects a stored entry
 * whose description differs, so a miss or a stale library only means the
 * pipeline is created normally. Implemented in rsx_d3d12_backend.c. */
#ifndef RSX_PSO_DISK_CACHE_H
#define RSX_PSO_DISK_CACHE_H

HRESULT rsx_pso_create_graphics(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                ID3D12PipelineState** out);
/* Once per presented frame: writes the library after new pipelines settle. */
void rsx_pso_cache_tick(void);

#endif

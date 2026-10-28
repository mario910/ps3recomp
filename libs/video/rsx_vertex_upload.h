#ifndef RSX_VERTEX_UPLOAD_H
#define RSX_VERTEX_UPLOAD_H
#include <stdint.h>
#include <string.h>
/* CPU staging remains 16 float4s per vertex. GPU streams retain the original
 * ATTR index, with one tightly packed float4 stream for each reflected input.
 * mask=0 is the unchanged interleaved layout for the legacy fallback PSO. */
typedef struct {uint32_t offset,bytes,mask;} RsxVertexUpload;
static inline unsigned rsx_vertex_upload_inputs(uint32_t mask)
{
    unsigned n=0;for(;mask;mask>>=1)n+=mask&1u;return n;
}
static inline int rsx_vertex_upload_pack(uint8_t* destination,uint32_t capacity,
    uint32_t* cursor,const uint8_t* source,uint64_t source_bytes,
    uint32_t count,uint32_t mask,RsxVertexUpload* upload)
{
    if(!destination || !cursor || !source || !upload || !count || (mask&~0xffffu) ||
       (*cursor&15u) || (uint64_t)count*256u>source_bytes)return 0;
    uint64_t bytes=(uint64_t)count*(mask?rsx_vertex_upload_inputs(mask)*16u:256u);
    if(!bytes || (uint64_t)*cursor+bytes>capacity)return 0;
    uint8_t* output=destination+*cursor;
    if(!mask)memcpy(output,source,(size_t)bytes);
    else for(unsigned attr=0;attr<16;++attr)if(mask&(1u<<attr)) {
        for(uint32_t vertex=0;vertex<count;++vertex)
            memcpy(output+(uint64_t)vertex*16u,source+(uint64_t)vertex*256u+attr*16u,16);
        output+=(uint64_t)count*16u;
    }
    upload->offset=*cursor;upload->bytes=(uint32_t)bytes;upload->mask=mask;
    *cursor+=(uint32_t)bytes;return 1;
}
/* For stream ATTRn; caller must first check that its bit is in mask. */
static inline uint32_t rsx_vertex_upload_stream(const RsxVertexUpload* upload,
    uint32_t count,unsigned attr)
{
    return upload->offset+rsx_vertex_upload_inputs(upload->mask&((1u<<attr)-1u))*count*16u;
}
#endif

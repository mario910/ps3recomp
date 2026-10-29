#ifndef RSX_PIPELINE_INDEX_H
#define RSX_PIPELINE_INDEX_H
#include <stdint.h>
#include <string.h>
/* Candidate index only: callers MUST compare the entire pipeline key.
 * Zero-initialized links use slot+1, reserving zero for end-of-chain. */
#define RSX_PSO_INDEX_BUCKETS 1024
#define RSX_PSO_INDEX_SLOTS 4096
typedef struct {
    uint16_t heads[RSX_PSO_INDEX_BUCKETS];
    uint16_t next[RSX_PSO_INDEX_SLOTS];
} rsx_pipeline_index;
static inline unsigned rsx_pipeline_bucket(uint32_t code, uint32_t vertex,
                                            uint32_t blend, uint32_t depth) {
    uint32_t h = code ^ (vertex * 0x9e3779b9u) ^ (blend * 0x85ebca6bu) ^ depth;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
    return h & (RSX_PSO_INDEX_BUCKETS - 1);
}
static inline void rsx_pipeline_index_clear(rsx_pipeline_index* index) {
    memset(index, 0, sizeof *index);
}
static inline void rsx_pipeline_index_add(rsx_pipeline_index* index,
                                          unsigned bucket, unsigned slot) {
    if (bucket >= RSX_PSO_INDEX_BUCKETS || slot >= RSX_PSO_INDEX_SLOTS) return;
    index->next[slot] = index->heads[bucket];
    index->heads[bucket] = (uint16_t)(slot + 1);
}
#endif

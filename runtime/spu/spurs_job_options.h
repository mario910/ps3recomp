#ifndef PS3_SPURS_JOB_OPTIONS_H
#define PS3_SPURS_JOB_OPTIONS_H
#include <stdlib.h>
typedef struct {
    int initialized, kernel, desc_dump, desc_detail, dump, stack, trace;
    int min_present, min_image, stage_present, stage_image;
} SpursJobOptions;
static inline void spurs_job_options_load(SpursJobOptions* out)
{
    const char* desc = getenv("SPURS_JOB_DESCDUMP");
    const char* min = getenv("SPURS_JOB_ABI_MIN_IMAGE");
    const char* stage = getenv("SPURS_JOB_ABI_STAGE_IMAGE");
    out->kernel = getenv("SPURS_JOB_KERNEL_MIRROR") != NULL;
    out->desc_dump = desc != NULL;
    out->desc_detail = desc ? atoi(desc) : 0;
    out->dump = getenv("SPURS_JOB_DUMP") != NULL;
    out->stack = getenv("SPURS_JOB_ABI_STACK") != NULL;
    out->trace = getenv("SPURS_JOB_TRACE") != NULL;
    out->min_present = min != NULL; out->min_image = min ? atoi(min) : 0;
    out->stage_present = stage != NULL; out->stage_image = stage ? atoi(stage) : 0;
    out->initialized = 1;
}
/* Startup-only option caching is explicit. SDK fixtures and callers that
 * change environment variables between jobs retain the live path. No saved
 * CRT string pointers, allocations, or cross-thread initialization guards. */
static inline const SpursJobOptions* spurs_job_options_for_call(
    SpursJobOptions* cache, SpursJobOptions* scratch, int startup_only)
{
    if (startup_only) {
        if (!cache->initialized) spurs_job_options_load(cache);
        return cache;
    }
    spurs_job_options_load(scratch);
    return scratch;
}
#endif

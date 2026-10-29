#ifndef PS3_PPU_TLS_IMAGE_H
#define PS3_PPU_TLS_IMAGE_H
#include <stdint.h>
#include <string.h>

/* Pure bounded copy, also exercised with synthetic TLS layouts by ports. */
static inline int ps3_ppu_tls_copy_image(uint8_t* vm, uint64_t limit,
    uint32_t destination, uint32_t capacity, uint32_t source,
    uint32_t filesz, uint32_t memsz)
{
    if (!vm || filesz > memsz || memsz > capacity ||
        (uint64_t)destination + capacity > limit || (uint64_t)source + filesz > limit)
        return -1;
    if (filesz && (uint64_t)source < (uint64_t)destination + capacity &&
        (uint64_t)destination < (uint64_t)source + filesz)
        return -1;
    memset(vm + destination, 0, capacity);
    if (filesz) memcpy(vm + destination, vm + source, filesz);
    return 0;
}

#ifdef __cplusplus
extern "C" {
#endif
/* Initialize caller-owned guest TLS from the ELF actually loaded, not a title address. */
int ppu_tls_copy_template(uint32_t destination, uint32_t capacity);
#ifdef __cplusplus
}
#endif
#endif

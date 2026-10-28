#ifndef SPURS_COMMAND_WAIT_H
#define SPURS_COMMAND_WAIT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* A notification is only a hint. The actual command and published chain PC
 * are always checked under the registration lock before parking. */
extern uint64_t g_spurs_command_wait_lines;
void spurs_command_written(uint32_t address);
/* SPU DMA PUT of [ea, ea+size): may publish a command over a parked JTS. */
void spurs_command_dma_written(uint32_t ea, uint32_t size);
void spurs_command_wait(const uint8_t* memory, uint32_t pc, uint32_t chain,
                        uint64_t published_pc, unsigned timeout_ms);
static inline uint64_t spurs_command_line_bit(uint32_t address)
{
    return 1ull << (((address >> 7) ^ (address >> 13)) & 63);
}
static inline void spurs_command_notify64(uint32_t address)
{
    if (__atomic_load_n(&g_spurs_command_wait_lines, __ATOMIC_ACQUIRE) &
        spurs_command_line_bit(address)) spurs_command_written(address);
}
#ifdef __cplusplus
}
#endif
#endif

#ifndef PSXRECOMP_V4_SPU_H
#define PSXRECOMP_V4_SPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spu_init(void);

/* MMIO read/write (0x1F801C00-0x1F801FFF) */
uint32_t spu_read(uint32_t addr);
void spu_write(uint32_t addr, uint32_t value);

/* DMA channel 4 interface */
void spu_dma_write(uint32_t word);
int spu_dma_ready(void);

/* Get pointer to SPU RAM for direct access (512KB) */
const uint8_t* spu_get_ram(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_SPU_H */

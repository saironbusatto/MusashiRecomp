#ifndef PSXRECOMP_V4_SPU_H
#define PSXRECOMP_V4_SPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spu_init(void);
void spu_render(int16_t* out_stereo, int frames);

typedef struct SpuDebugInfo {
    uint32_t ctrl;
    uint32_t active_mask;
    int16_t main_l;
    int16_t main_r;
    uint32_t key_on_count;
    uint64_t render_frames;
    uint64_t nonzero_frames;
    int32_t last_peak;
    int32_t peak;
} SpuDebugInfo;

void spu_debug_info(SpuDebugInfo* out);

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

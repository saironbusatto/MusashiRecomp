#ifndef PSXRECOMP_V4_CDROM_H
#define PSXRECOMP_V4_CDROM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void cdrom_init(const char* cue_path);

/* MMIO read/write (0x1F801800-0x1F801803) */
uint32_t cdrom_read(uint32_t addr);
void cdrom_write(uint32_t addr, uint32_t value);

/* Advance CD-ROM state machine. Call per-frame. Sets i_stat IRQ_CDROM
 * when a command completes or data is ready. */
void cdrom_tick(void);

/* DMA channel 3 interface */
uint32_t cdrom_dma_read(void);
int cdrom_dma_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_CDROM_H */

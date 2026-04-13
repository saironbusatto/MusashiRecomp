/*
 * spu.c — PS1 Sound Processing Unit register-level simulation
 *
 * Accepts all SPU register reads/writes at 0x1F801C00-0x1F801FFF and
 * DMA4 transfers into 512KB SPU RAM. No audio synthesis yet — this
 * exists so the BIOS and games can write SPU registers without crashing.
 *
 * Audio output will be added in a later phase using SDL_audio with the
 * ADPCM decoder from psxrecomp-v2/runner/src/spu.cpp as reference.
 *
 * Reference: nocash PSX specs — SPU section
 * Register map: v2 spu.cpp header comment (voice regs, global regs)
 */

#include "spu.h"
#include <string.h>

#define SPU_RAM_SIZE  (512 * 1024)
#define SPU_REG_COUNT 256  /* 0x1F801C00-0x1F801DFF as uint16_t[256] */

static uint8_t  spu_ram[SPU_RAM_SIZE];
static uint16_t spu_regs[SPU_REG_COUNT];
static uint32_t transfer_addr;  /* byte address in SPU RAM */

void spu_init(void) {
    memset(spu_ram, 0, sizeof(spu_ram));
    memset(spu_regs, 0, sizeof(spu_regs));
    transfer_addr = 0;
}

uint32_t spu_read(uint32_t addr) {
    if (addr >= 0x1F801C00u && addr <= 0x1F801DFFu) {
        uint32_t idx = (addr - 0x1F801C00u) >> 1;
        if (idx < SPU_REG_COUNT) {
            /* SPUSTAT (0x1F801DAE): report ready, not busy */
            if (addr == 0x1F801DAEu) {
                return 0x0000;
            }
            return spu_regs[idx];
        }
    }
    /* 0x1F801E00-0x1F801FFF: voice current volume (read-only, return 0) */
    return 0;
}

void spu_write(uint32_t addr, uint32_t value) {
    if (addr >= 0x1F801C00u && addr <= 0x1F801DFFu) {
        uint32_t idx = (addr - 0x1F801C00u) >> 1;
        if (idx < SPU_REG_COUNT) {
            spu_regs[idx] = (uint16_t)value;

            /* Transfer Address register (0x1F801DA6):
             * value = byte_addr >> 3 (units of 8 bytes) */
            if (addr == 0x1F801DA6u) {
                transfer_addr = ((uint32_t)(uint16_t)value) << 3;
                if (transfer_addr >= SPU_RAM_SIZE)
                    transfer_addr = 0;
            }

            /* Transfer Data FIFO (0x1F801DA8): write 16-bit to SPU RAM */
            if (addr == 0x1F801DA8u) {
                if (transfer_addr + 1 < SPU_RAM_SIZE) {
                    spu_ram[transfer_addr]     = (uint8_t)(value & 0xFF);
                    spu_ram[transfer_addr + 1] = (uint8_t)((value >> 8) & 0xFF);
                }
                transfer_addr = (transfer_addr + 2) % SPU_RAM_SIZE;
            }
        }
    }
}

void spu_dma_write(uint32_t word) {
    /* DMA4 writes 32-bit words (two 16-bit samples) to SPU RAM */
    if (transfer_addr + 3 < SPU_RAM_SIZE) {
        spu_ram[transfer_addr]     = (uint8_t)(word & 0xFF);
        spu_ram[transfer_addr + 1] = (uint8_t)((word >> 8) & 0xFF);
        spu_ram[transfer_addr + 2] = (uint8_t)((word >> 16) & 0xFF);
        spu_ram[transfer_addr + 3] = (uint8_t)((word >> 24) & 0xFF);
    }
    transfer_addr = (transfer_addr + 4) % SPU_RAM_SIZE;
}

int spu_dma_ready(void) {
    return 1; /* always ready — synchronous model */
}

const uint8_t* spu_get_ram(void) {
    return spu_ram;
}

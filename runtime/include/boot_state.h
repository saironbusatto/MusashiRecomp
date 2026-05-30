#ifndef PSX_BOOT_STATE_H
#define PSX_BOOT_STATE_H

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_STATE_MAGIC   0x50535842u  /* "PSXB" */
#define BOOT_STATE_VERSION 1u

/* Serialized header — written verbatim to disk, followed by 2 MB RAM blob. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t bios_checksum; /* sum of all uint32_t words in BIOS ROM */
    uint32_t entry_pc;

    uint32_t gpr[32];
    uint32_t pc;
    uint32_t hi, lo;
    uint32_t cop0[32];
    uint32_t gte_data[32];
    uint32_t gte_ctrl[32];

    uint32_t i_stat;
    uint32_t i_mask;

    uint16_t timer_counter[3];
    uint8_t  _pad0[2];
    uint32_t timer_mode[3];
    uint16_t timer_target[3];
    uint8_t  _pad1[2];
    int32_t  timer_irq_line[3];
    uint32_t timer_frac[3];

    uint8_t  scratchpad[1024];
    uint32_t dirty_bitmap[16]; /* 512 4 KB pages / 32 bits per word */
    uint8_t  _pad2[4];
} BootStateHeader;

/* Save full system state at game handoff. Returns 1 on success. */
int  boot_state_save(const CPUState* cpu, uint32_t bios_checksum,
                     uint32_t entry_pc, const char* path);

/* Load and validate snapshot; restores CPU/RAM/hardware. Returns 1 if valid. */
int  boot_state_load(const char* path, uint32_t bios_checksum,
                     uint32_t entry_pc, CPUState* cpu);

/* Register a deferred capture: when boot_state_trigger_capture() is called
 * (from fntrace at game-start), serialize state to path. One-shot. */
void boot_state_set_capture(const char* path, uint32_t bios_checksum,
                             uint32_t entry_pc);

/* Called from fntrace when game entry PC first fires. */
void boot_state_trigger_capture(const CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* PSX_BOOT_STATE_H */

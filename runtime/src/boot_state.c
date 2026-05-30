#include "boot_state.h"
#include <stdio.h>
#include <string.h>

#define RAM_SIZE (2u * 1024u * 1024u)

/* Accessors provided by other runtime modules. */
extern uint8_t*  memory_get_ram_ptr(void);
extern uint8_t*  memory_get_scratchpad_ptr(void);
extern uint32_t  dirty_ram_get_bitmap_word(uint32_t word_index);
extern uint32_t  dirty_ram_get_bitmap_word_count(void);
extern void      dirty_ram_set_bitmap_words(const uint32_t* words, uint32_t count);
extern uint32_t  i_stat;
extern uint32_t  i_mask;
extern void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                                uint16_t target[3], int32_t  irq_line[3],
                                uint32_t frac[3]);
extern void timers_set_snapshot(const uint16_t counter[3], const uint32_t mode[3],
                                const uint16_t target[3], const int32_t  irq_line[3],
                                const uint32_t frac[3]);

/* Deferred capture state — set before first boot, fired at game handoff. */
static char     s_capture_path[512];
static uint32_t s_capture_checksum;
static uint32_t s_capture_entry_pc;

void boot_state_set_capture(const char* path, uint32_t bios_checksum,
                             uint32_t entry_pc) {
    strncpy(s_capture_path, path, sizeof(s_capture_path) - 1);
    s_capture_path[sizeof(s_capture_path) - 1] = '\0';
    s_capture_checksum = bios_checksum;
    s_capture_entry_pc = entry_pc;
}

int boot_state_save(const CPUState* cpu, uint32_t bios_checksum,
                    uint32_t entry_pc, const char* path) {
    BootStateHeader h;
    memset(&h, 0, sizeof(h));

    h.magic         = BOOT_STATE_MAGIC;
    h.version       = BOOT_STATE_VERSION;
    h.bios_checksum = bios_checksum;
    h.entry_pc      = entry_pc;

    memcpy(h.gpr,      cpu->gpr,      sizeof(h.gpr));
    h.pc = cpu->pc;
    h.hi = cpu->hi;
    h.lo = cpu->lo;
    memcpy(h.cop0,     cpu->cop0,     sizeof(h.cop0));
    memcpy(h.gte_data, cpu->gte_data, sizeof(h.gte_data));
    memcpy(h.gte_ctrl, cpu->gte_ctrl, sizeof(h.gte_ctrl));

    h.i_stat = i_stat;
    h.i_mask = i_mask;

    timers_get_snapshot(h.timer_counter, h.timer_mode, h.timer_target,
                        h.timer_irq_line, h.timer_frac);

    memcpy(h.scratchpad, memory_get_scratchpad_ptr(), 1024);

    uint32_t bw = dirty_ram_get_bitmap_word_count();
    if (bw > 16) bw = 16;
    for (uint32_t i = 0; i < bw; i++)
        h.dirty_bitmap[i] = dirty_ram_get_bitmap_word(i);

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "boot_state: cannot open %s for write\n", path);
        return 0;
    }
    int ok = (fwrite(&h, sizeof(h), 1, f) == 1) &&
             (fwrite(memory_get_ram_ptr(), RAM_SIZE, 1, f) == 1);
    fclose(f);
    if (ok)
        fprintf(stdout, "boot_state: snapshot saved -> %s\n", path);
    else
        fprintf(stderr, "boot_state: write error for %s\n", path);
    return ok;
}

int boot_state_load(const char* path, uint32_t bios_checksum,
                    uint32_t entry_pc, CPUState* cpu) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    BootStateHeader h;
    int ok = (fread(&h, sizeof(h), 1, f) == 1);
    if (ok) ok = (fread(memory_get_ram_ptr(), RAM_SIZE, 1, f) == 1);
    fclose(f);

    if (!ok ||
        h.magic         != BOOT_STATE_MAGIC   ||
        h.version       != BOOT_STATE_VERSION  ||
        h.bios_checksum != bios_checksum       ||
        h.entry_pc      != entry_pc) {
        if (ok)  /* file exists but is stale */
            fprintf(stdout, "boot_state: snapshot stale, will rebuild %s\n", path);
        return 0;
    }

    memcpy(cpu->gpr,      h.gpr,      sizeof(h.gpr));
    cpu->pc = h.entry_pc;   /* always jump to entry_pc, never saved mid-PC */
    cpu->hi = h.hi;
    cpu->lo = h.lo;
    memcpy(cpu->cop0,     h.cop0,     sizeof(h.cop0));
    memcpy(cpu->gte_data, h.gte_data, sizeof(h.gte_data));
    memcpy(cpu->gte_ctrl, h.gte_ctrl, sizeof(h.gte_ctrl));

    i_stat = h.i_stat;
    i_mask = h.i_mask;

    timers_set_snapshot(h.timer_counter, h.timer_mode, h.timer_target,
                        h.timer_irq_line, h.timer_frac);

    memcpy(memory_get_scratchpad_ptr(), h.scratchpad, 1024);

    uint32_t bw = dirty_ram_get_bitmap_word_count();
    if (bw > 16) bw = 16;
    dirty_ram_set_bitmap_words(h.dirty_bitmap, bw);

    fprintf(stdout, "boot_state: restored snapshot, jumping to 0x%08X\n", entry_pc);
    return 1;
}

void boot_state_trigger_capture(const CPUState* cpu) {
    if (!s_capture_path[0]) return;
    boot_state_save(cpu, s_capture_checksum, s_capture_entry_pc, s_capture_path);
    s_capture_path[0] = '\0';
}

/* dirty_ram_interp.h — interpret-on-dispatch for install-at-runtime RAM.
 *
 * See CLAUDE.md Rule 18 and docs/dynamic_handler_install.md for the full
 * rationale.  The PS1 BIOS dynamically writes 4-instruction dispatch stubs
 * into kernel RAM (notably RAM 0xCF0 for the SIO data-byte handler).  A
 * static recompiler can't see those bytes at compile time, so a small MIPS
 * interpreter here runs them at dispatch time on the same CPUState.
 *
 * Scope: this is NOT a fallback for code the recompiler failed to translate.
 * It runs only against PCs in pages that have been written-to since boot.
 * Static-recompiled code continues to handle ROM-resident code and game
 * RAM.  See docs/dynamic_handler_install.md for the inline note about a
 * potential future migration to runtime JIT (Option B).
 */
#ifndef PSXRECOMP_V4_DIRTY_RAM_INTERP_H
#define PSXRECOMP_V4_DIRTY_RAM_INTERP_H

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 if `addr` lies in a dirty kernel-RAM page and the interpreter
 * ran a basic block at that PC.  Returns 0 if `addr` is clean (caller must
 * fall back to the static dispatch table).  On return with 1, cpu->pc is
 * either 0 (block ended on jr $ra style return) or the target of a tail
 * jump that the dispatch trampoline should re-enter. */
int dirty_ram_dispatch(CPUState* cpu, uint32_t addr);

/* Test whether a given physical kernel-RAM address is in a page that was
 * written-to since boot.  Defined in memory.c. */
int      dirty_ram_is_dirty(uint32_t phys);
uint32_t dirty_ram_get_bitmap(void);

/* Counters for visibility / TCP debug.  Increment in interpreter; expose
 * via debug_server.c if helpful. */
extern uint64_t g_dirty_ram_blocks_run;     /* basic blocks interpreted */
extern uint64_t g_dirty_ram_insns_run;      /* instructions interpreted */
extern uint64_t g_dirty_ram_aborts;         /* unsupported-opcode aborts */

/* Per-entry-PC counters.  Aggregate counters above hide which install
 * stubs actually fire — a single noisy spurious-dispatch site can mask
 * a legitimate handler that never runs.  This open-addressed table keys
 * on the entry PC of each interpreted block. */
#define DIRTY_RAM_PC_TABLE_SIZE 64
typedef struct {
    uint32_t pc;        /* entry PC, 0 = empty slot */
    uint64_t hits;      /* number of times dispatched here */
    uint64_t insns;     /* total instructions executed across hits */
} DirtyRamPcEntry;
extern DirtyRamPcEntry g_dirty_ram_pc_table[DIRTY_RAM_PC_TABLE_SIZE];

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_DIRTY_RAM_INTERP_H */

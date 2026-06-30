/* lockstep.h — #2 lockstep "unit-test interp" comparator.
 *
 * Per-basic-block differential check: the compiled block runs (RECORD mode,
 * its memory ops captured), then the SAME block is re-interpreted from the
 * same entry state (REPLAY mode, memory ops fed from the recorded trace so the
 * replay is side-effect-free and MMIO-safe). If the interp replay reads a
 * different address, writes a different value, or ends with different
 * registers, that block's compiled output is wrong -> the exact codegen defect.
 * If NO block ever diverges, the regression is timing/IRQ-phase, not codegen.
 *
 * Dev tooling only: the per-block hook is debug_server_cyc_observe, which is
 * PSX_NO_DEBUG_TOOLS-stripped in release builds. Window-gated (frame range) so
 * it's off (full speed) outside the region of interest.
 */
#pragma once
#include <stdint.h>
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 0=off, 1=record (compiled block running), 2=replay (interp re-run). */
extern int g_ls_mode;
/* 1 while replaying: cycle/icache accounting no-ops so the replay never
 * perturbs real global timing state. */
extern int g_ls_replay_active;

/* Memory chokepoint hooks, called from the psx_read_ and psx_write_ funcs. */
uint32_t ls_read_hook(uint32_t addr, int size, uint32_t real_val);
void     ls_write_hook(uint32_t addr, int size, uint32_t val);

/* Per-basic-block-leader entry, called from debug_server_cyc_observe. */
void     ls_at_leader(uint32_t leader_phys, CPUState *cpu);

/* Arm the comparator over a guest-frame window [lo,hi]. hi==0 => disabled. */
void     ls_set_window(uint32_t frame_lo, uint32_t frame_hi);

/* Diagnostic: record blocks but skip the inline replay (to test whether the
 * inline replay, not recording, is what perturbs the run). */
void     ls_set_record_only(int on);

/* Drain the first-divergence record as JSON into buf. Returns bytes written. */
int      ls_get_diverge_json(char *buf, int buflen);

#ifdef __cplusplus
}
#endif

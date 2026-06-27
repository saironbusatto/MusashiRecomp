/* psx_instr_cost.h — SINGLE SOURCE OF TRUTH for R3000A guest CPU cycle cost.
 *
 * FAITHFUL_TIMING_PLAN.md P3/Stage-2. Both execution backends — the dirty-RAM
 * interpreter (runtime/src/dirty_ram_interp.c) and the static recompiler
 * (recompiler/src/code_generator.cpp, which sums this over a block at gen time
 * to fold it into one compile-time block charge) — MUST account guest cycles
 * THROUGH THIS FUNCTION so they can never disagree (the -8 drift class).
 *
 * STAGE 1 (now): identity, 1 cycle per instruction. This is the proven baseline
 * both backends already used, so routing them through here is BEHAVIOR-PRESERVING
 * (a regen must be byte-identical) and merely establishes the seam.
 *
 * STAGE 2 (next): replace the body with the documented R3000A model, transcribed
 * (facts, not code) from the in-tree oracle psxrecomp/beetle-psx/mednafen/psx/
 * (cpu.cpp MULT_Tab/muldiv, gte.cpp GTE_Instruction per-command table) and
 * psx-spx, each value VERIFIED against Beetle at runtime before it is trusted.
 * Memory-access wait-states are charged separately in the psx_read/write path
 * (they are address/region-dependent, not knowable from the opcode alone), so
 * this function returns ONLY the CPU instruction/execute base cost. Keep that
 * split clean in both backends to avoid double-counting.
 *
 * Header is plain C (static inline) so both the C runtime and the C++ recompiler
 * can include it as the one definition.
 */
#ifndef PSX_INSTR_COST_H
#define PSX_INSTR_COST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU instruction base cycle cost (execute cycles only; excludes data-access
 * wait states, which the memory path charges). `insn` is the raw 32-bit MIPS
 * word. Stage 1: identity. */
static inline uint32_t psx_instr_base_cycles(uint32_t insn) {
    (void)insn;
    return 1u;
}

#ifdef __cplusplus
}
#endif

#endif /* PSX_INSTR_COST_H */

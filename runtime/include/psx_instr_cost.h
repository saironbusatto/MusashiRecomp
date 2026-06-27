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

/* Guest CPU cycle cost for one retired instruction. `insn` is the raw 32-bit MIPS
 * word. Transcribed from the in-tree Beetle oracle (mednafen psx/cpu.cpp) + psx-spx;
 * each component verified against Beetle via the cyc_watch DELTA gate
 * (FAITHFUL_TIMING_PLAN.md §3c). Both backends consume this one function.
 *
 * STAGE 2 component #1 — data-memory access (the dominant ~2x gap):
 *   Beetle PS_CPU::ReadMemory: a normal CPU load adds `lts += 2` (LWC loads +1);
 *   stores are POSTED (WriteMemory adds no CPU stall); scratchpad reads add 0.
 *   So: load opcode = 1 (execute) + 2 (data access) = 3; LWC2 = 2; store/other = 1.
 * NOT YET MODELED (later Δ-gated components, will adjust here / in the mem path):
 *   - scratchpad-vs-RAM region (scratchpad load should be 1, not 3),
 *   - the read-after-load "fudge" (+0/2) and load-delay ABSORB (next-insn discount),
 *   - I-cache instruction-fetch timing, mult/div latency, GTE per-command costs.
 *   This RAM-load base is an upper bound on the load component (no absorb yet); the
 *   Δ gate shows whether we under/overshoot and what to add next. */
static inline uint32_t psx_instr_base_cycles(uint32_t insn) {
    uint32_t op = insn >> 26;
    switch (op) {
        case 0x20: case 0x21: case 0x22: case 0x23:   /* LB  LH  LWL LW  */
        case 0x24: case 0x25: case 0x26:              /* LBU LHU LWR     */
            return 3u;                                /* 1 execute + 2 data-access */
        case 0x32:                                    /* LWC2 (GTE load): +1 */
            return 2u;
        default:
            return 1u;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* PSX_INSTR_COST_H */

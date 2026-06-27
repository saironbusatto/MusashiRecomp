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
 * DATA-MEMORY ACCESS IS *NOT* CHARGED HERE — it lives in the load-delay interlock.
 *   Guest CPU data loads route through psx_cyc_load_* (runtime/src/memory.c, declared
 *   in psx_cyc.h), which charges the Beetle ReadMemory cost address-keyed (main RAM
 *   region +3, completion +2, scratchpad +0) as the LDAbsorb give-back, plus the +2
 *   ReadFudge — exactly like PS_CPU::ReadMemory, NOT in the opcode dispatch. The
 *   opcode word alone cannot know the target region (RAM vs scratchpad vs MMIO), so a
 *   per-opcode load surcharge HERE is wrong by construction and would double-count.
 *   NOTE: in per-instruction mode (the default) the §1 base is charged by psx_cyc_base
 *   (psx_cyc.h), not by this function — psx_instr_base_cycles is only the block-mode
 *   (PSX_CODEGEN_CYCLE_PER_INSN=0) fallback sum and the dep/res mask seed below.
 *
 *   HISTORY: Stage-2 #1a (commit 2ef47bd) returned 3 for loads / 2 for LWC2
 *   ("1 execute + 2 data-access"). The BIOS-kernel cycle ruler (region
 *   [0x80001C5C→0x80001CA4]) proved this double-counts: native charged the 2
 *   loads +2 each HERE *and* +6 each in memory.c (8 over base), reconciling the
 *   opaque 0x80017FC4 window only because that over-charge masked the entirely-
 *   unmodeled divu→mflo stall. Reverted to pure execute base (this header's own
 *   stated contract). The real Stage-2 components are EXECUTE-pipeline latencies
 *   (mult/div stall, GTE per-command) added here, and CALIBRATING the memory-path
 *   wait-state (memory.c) per Beetle region — each Δ-gated on the ruler. */
static inline uint32_t psx_instr_base_cycles(uint32_t insn) {
    (void)insn;
    return 1u;   /* pure CPU execute base; data access charged in memory.c */
}

/* GPR dep+res bitmask (bit n => GPR n) for one MIPS-I instruction, transcribed
 * from the Beetle oracle's per-opcode GPR_DEP/GPR_RES (cpu.cpp). Drives the
 * load-delay interlock's GPR_DEPRES step (psx_cyc_step / psx_cyc_load_*, psx_cyc.h)
 * — the give-back ends when a following instruction reads/writes the loaded reg.
 * For CPU loads this returns the source (rs) ONLY: the dest rt is the delayed-load
 * target (armed via LDWhich), not a GPR_RES. Pure (insn-only) so the static emitters
 * fold it to a literal at gen-time and the interpreter calls it at runtime. */
static inline uint32_t psx_cyc_dep_res_mask(uint32_t insn) {
    uint32_t op = insn >> 26;
    uint32_t rs = (insn >> 21) & 0x1Fu;
    uint32_t rt = (insn >> 16) & 0x1Fu;
    uint32_t rd = (insn >> 11) & 0x1Fu;
    uint32_t M_rs = 1u << rs, M_rt = 1u << rt, M_rd = 1u << rd;
    switch (op) {
    case 0x00: { /* SPECIAL */
        uint32_t fn = insn & 0x3Fu;
        switch (fn) {
        case 0x00: case 0x02: case 0x03:                 return M_rt | M_rd;        /* SLL/SRL/SRA */
        case 0x04: case 0x06: case 0x07:                 return M_rs | M_rt | M_rd; /* SLLV/SRLV/SRAV */
        case 0x08: case 0x09:                            return M_rs | M_rd;        /* JR/JALR */
        case 0x10: case 0x12:                            return M_rd;               /* MFHI/MFLO */
        case 0x11: case 0x13:                            return M_rs;               /* MTHI/MTLO */
        case 0x18: case 0x19: case 0x1A: case 0x1B:      return M_rs | M_rt;        /* MULT/MULTU/DIV/DIVU */
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x2A: case 0x2B:                            return M_rs | M_rt | M_rd; /* ADD..NOR/SLT/SLTU */
        default:                                         return 0u;                 /* SYSCALL/BREAK/... */
        }
    }
    case 0x01: { /* REGIMM/BCOND: DEP rs, RES link(31 for *AL variants) */
        uint32_t link = ((rt & 0x1Eu) == 0x10u) ? (1u << 31) : 0u;
        return M_rs | link;
    }
    case 0x02:                                           return 0u;                 /* J */
    case 0x03:                                           return 1u << 31;           /* JAL (RES 31) */
    case 0x04: case 0x05:                                return M_rs | M_rt;        /* BEQ/BNE */
    case 0x06: case 0x07:                                return M_rs;               /* BLEZ/BGTZ */
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E:                     return M_rs | M_rt;        /* ADDI..XORI */
    case 0x0F:                                           return M_rt;               /* LUI (RES rt) */
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26:                     return M_rs;               /* LB..LWR (load: rs only) */
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2E:                                           return M_rs | M_rt;        /* SB/SH/SWL/SW/SWR */
    /* COP0/1/2/3 (0x10-0x13), LWC* (0x30-0x33), SWC* (0x38-0x3B): no GPR_DEP/RES */
    default:                                             return 0u;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* PSX_INSTR_COST_H */

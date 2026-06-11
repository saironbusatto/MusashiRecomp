/* ============================================================================
 * cpu_state.h  —  Phase 2 runtime version
 * ----------------------------------------------------------------------------
 * This is the RUNTIME version of the CPUState header. It has the same struct
 * layout as generated/cpu_state.h (Phase 1a compile-only artifact) but is
 * intended to be linked into a runnable binary.
 *
 * The generated C files (SCPH1001_full.c, SCPH1001_dispatch.c) include
 * "cpu_state.h" and the build uses -I runtime/include so they find this file.
 * ========================================================================== */

#ifndef PSXRECOMP_CPU_STATE_H
#define PSXRECOMP_CPU_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPUState {
    uint32_t gpr[32];   /* $0..$31; gpr[0] is hardwired zero, never written */
    uint32_t pc;        /* program counter */
    uint32_t hi, lo;    /* mult/div result registers */
    uint32_t cop0[32];  /* COP0 system control registers (SR, Cause, EPC, ...) */
    uint32_t gte_data[32]; /* COP2 (GTE) data registers */
    uint32_t gte_ctrl[32]; /* COP2 (GTE) control registers */

    /* Memory access function pointers -- wired at init to psx_read/psx_write. */
    uint32_t (*read_word)(uint32_t addr);
    void     (*write_word)(uint32_t addr, uint32_t value);
    uint16_t (*read_half)(uint32_t addr);
    void     (*write_half)(uint32_t addr, uint16_t value);
    uint8_t  (*read_byte)(uint32_t addr);
    void     (*write_byte)(uint32_t addr, uint8_t value);
} CPUState;

/* Trap trampolines — defined in runtime/src/traps.c */
extern void psx_syscall(CPUState* cpu, uint32_t code);
extern void psx_arith_overflow(CPUState* cpu);
extern void psx_unaligned_access(CPUState* cpu, uint32_t addr, uint32_t pc);
extern void psx_break(CPUState* cpu, uint32_t code, uint32_t pc);

/* Dispatch — defined in generated/SCPH1001_dispatch.c */
extern void psx_dispatch(CPUState* cpu, uint32_t target_addr);
extern void psx_dispatch_call(CPUState* cpu, uint32_t target_addr, uint32_t return_addr);

/* Unknown dispatch — defined in runtime/src/traps.c */
extern void psx_unknown_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys);

/* GTE (COP2) — defined in runtime/src/gte.cpp */
extern void     gte_execute(CPUState* cpu, uint32_t cmd);
extern uint32_t gte_read_data(CPUState* cpu, uint8_t reg);
extern uint32_t gte_read_ctrl(CPUState* cpu, uint8_t reg);
extern void     gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);
extern void     gte_write_ctrl(CPUState* cpu, uint8_t reg, uint32_t val);

/* ============================================================================
 * Dispatch call contract (Bug D / wild-return family fix)
 * ----------------------------------------------------------------------------
 * Generated C continuations mirror the guest call graph: after `jal F`, the C
 * code calls F's C function and then falls into the continuation block.  Real
 * hardware has only one stack; if F's flow goes wild (returns to an address
 * other than the call site's $ra, or with a shifted $sp — e.g. a mid-function
 * entry running an epilogue for a frame it never pushed), the hardware simply
 * keeps executing wherever the guest jumped, and the interrupted caller's
 * code never resumes.  Our C model, by contrast, resumes the suspended C
 * continuation whenever the C call returns — re-executing tails on a moved
 * guest stack (the Bug A/C/D zombie family).
 *
 * The contract: a C continuation may run ONLY if the guest actually arrived
 * at it — i.e. the callee came back with $ra == the call site's return
 * address and $sp == the caller's stack pointer at the call.  When the
 * contract is violated, we begin a "bail" unwind: cpu->pc is set to the
 * guest's true target (the wild jr's destination), g_psx_call_bail is set,
 * and every generated frame returns immediately without running its
 * continuation.  The unwind resolves at the first enclosing call site (or
 * dispatch loop) whose (return address, sp) contract matches the guest's
 * arrival state; if none matches, the outermost dispatch loop clears the
 * flag and tail-dispatches the wild target with a clean host stack.
 *
 * g_psx_call_bail is defined in runtime/src/traps.c.  Overlay DLLs share
 * the runtime's state through pointers wired by overlay_init (the
 * PSX_OVERLAY_DLL_BUILD branch; see overlay_api.h / compile_overlays.py).
 * ========================================================================== */
#ifdef PSX_OVERLAY_DLL_BUILD
extern int      *g_psx_call_bail_p;
extern uint64_t *g_psx_bail_first_p;
extern uint64_t *g_psx_bail_resolved_p;
#define g_psx_call_bail     (*g_psx_call_bail_p)
#define g_psx_bail_first    (*g_psx_bail_first_p)
#define g_psx_bail_resolved (*g_psx_bail_resolved_p)
#else
extern int      g_psx_call_bail;
extern uint64_t g_psx_bail_first;      /* contract violations detected      */
extern uint64_t g_psx_bail_resolved;   /* unwinds resolved at a call site   */
extern uint64_t g_psx_bail_flattened;  /* unwinds flattened at outermost    */
extern uint64_t g_psx_bail_anomaly;    /* bail flag seen where impossible   */
#endif

/* Validate a direct call site after the callee's C return.
 * Returns 1 if the caller must `return;` immediately (bail in progress),
 * 0 if the continuation is valid.  site_ra = the call's return address,
 * site_sp = guest $sp recorded immediately before the call. */
static inline int psx_call_contract(CPUState* cpu, uint32_t site_ra,
                                    uint32_t site_sp) {
    if (g_psx_call_bail) {
        /* An inner frame began a bail unwind.  Resolve here iff the guest's
         * arrival state matches this site's contract. */
        if (((cpu->pc ^ site_ra) & 0x1FFFFFFFu) == 0 &&
            cpu->gpr[29] == site_sp) {
            g_psx_call_bail = 0;
            g_psx_bail_resolved++;
            cpu->pc = 0;
            return 0;
        }
        return 1;
    }
    if (cpu->gpr[29] != site_sp ||
        ((cpu->gpr[31] ^ site_ra) & 0x1FFFFFFFu) != 0) {
        /* First detection: the callee C-returned but the guest did not
         * return here.  $ra holds the wild jr's true destination (the
         * longjmp-return emission sets cpu->pc = $ra before returning,
         * which is the same value). */
        g_psx_call_bail = 1;
        g_psx_bail_first++;
        cpu->pc = cpu->gpr[31];
        return 1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_CPU_STATE_H */

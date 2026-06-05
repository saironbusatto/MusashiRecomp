#ifndef PSXRECOMP_INTERRUPTS_H
#define PSXRECOMP_INTERRUPTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* IRQ bit positions in I_STAT/I_MASK */
#define IRQ_VBLANK  0
#define IRQ_GPU     1
#define IRQ_CDROM   2
#define IRQ_DMA     3
#define IRQ_TIMER0  4
#define IRQ_TIMER1  5
#define IRQ_TIMER2  6
#define IRQ_SIO0    7
#define IRQ_SIO1    8
#define IRQ_SPU     9
#define IRQ_PIO     10

void interrupts_init(void);

/* Called from the dispatch loop after each function returns.
 * Fires vblank on schedule, checks (i_stat & i_mask), and if
 * pending + COP0 allows, dispatches the exception handler. */
void psx_check_interrupts(struct CPUState* cpu);
void psx_check_interrupts_at(struct CPUState* cpu, uint32_t resume_pc);

/* Accumulate emitted PSX cycles toward the next VBlank trigger.
 * Called from psx_advance_cycles() so the VBlank rate is gated on
 * guest cycles (correct PSX timing) rather than block-dispatch
 * count (which was 5-6x too fast and squeezed game-time to ~60% of
 * real). One real-PSX VBlank = 564480 cycles (33.8688 MHz / 60). */
void interrupts_advance_cycles(uint32_t cycles);

/* Query whether we are currently inside an exception handler dispatch. */
int psx_get_in_exception(void);

/* Snapshot internal counters for the freeze_check diagnostic.  Any out_*
 * pointer may be NULL.  All counters are monotonically non-decreasing
 * (dispatch_count resets each VBlank). */
void psx_get_freeze_diag(uint64_t *out_total_checks,
                         uint32_t *out_dispatch_count,
                         int *out_in_exception,
                         int *out_post_exc_cooldown,
                         uint64_t *out_exc_entries,
                         uint64_t *out_exc_reentry_blocks);

#define PSX_IRQ_CHECK_TRACE_CAP (1u << 18)
typedef struct PsxIrqCheckTraceEntry {
    uint64_t seq;
    uint32_t frame;
    uint32_t resume_pc;
    uint32_t i_stat;
    uint32_t i_mask;
    uint32_t sr;
    uint32_t cause;
    uint32_t call_depth;
    uint32_t reason;
} PsxIrqCheckTraceEntry;

extern PsxIrqCheckTraceEntry g_psx_irq_check_trace[PSX_IRQ_CHECK_TRACE_CAP];
extern uint64_t g_psx_irq_check_trace_seq;
extern uint64_t g_psx_nonlocal_epoch;

/* Bumped whenever BIOS exception/RestoreState/RFE flow abandons the current
 * generated C call stack. Dispatch uses this to avoid consuming a synthetic
 * JAL/JALR return address as a normal inline return after guest context has
 * been restored elsewhere. */
void psx_note_nonlocal_control_flow(void);

/* longjmp back to psx_check_interrupts, unwinding the exception handler.
 * Called by ReturnFromException (B0:0x17 or SYSCALL(3)) when inside
 * the exception handler to model real hardware's RFE+JR $k0 unwind. */
void psx_exception_longjmp(void);
void psx_rfe_return(struct CPUState* cpu);

/* longjmp back to psx_check_interrupts for RestoreState redirection.
 *
 * On real hardware, RestoreState (A0:0x14) restores all GPRs from a
 * save buffer and does `jr $ra`, which jumps to the restored $ra —
 * abandoning the current call stack.  In our model, a plain `return;`
 * would go back to the C caller (the chain walker), not to $ra.
 *
 * This function longjmps with code 2 (vs. 1 for ReturnFromException),
 * so psx_check_interrupts can re-dispatch to cpu->pc while still in
 * exception context.  The redirected code (e.g., VSync callback loop)
 * will eventually call ReturnFromException (longjmp code 1) to exit. */
void psx_restore_state_escape(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_INTERRUPTS_H */

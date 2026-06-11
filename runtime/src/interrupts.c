/*
 * interrupts.c — v4 interrupt delivery for recompiled BIOS.
 *
 * Pure hardware simulation. No BIOS state, no HLE, no interpreter.
 *
 * Since recompiled code runs as native C (no per-instruction stepping),
 * interrupt delivery happens at dispatch loop boundaries. The dispatch
 * loop calls psx_check_interrupts() after each function returns.
 *
 * Vblank is fired on a dispatch-count schedule to approximate 60 Hz.
 *
 * ReturnFromException handling:
 *   On real hardware, ReturnFromException (B0:0x17 or SYSCALL(3))
 *   restores the full register context from the TCB and jumps to the
 *   saved EPC.  This effectively "longjmps" out of the exception
 *   handler, bypassing any remaining chain-walk code.
 *
 *   In our recompiled model the exception handler runs as a nested
 *   psx_dispatch call.  A normal function return would unwind only one
 *   frame, leaving the chain walker running with corrupted registers.
 *   We use setjmp/longjmp to model the real hardware behaviour:
 *   psx_check_interrupts sets a jump point before dispatching the
 *   handler, and psx_exception_longjmp() (called by the runtime's
 *   ReturnFromException implementation) longjmps back, unwinding the
 *   entire handler call tree in one step.
 */

#include "interrupts.h"
#include "sio.h"
#include "timers.h"
#include "gpu.h"
#include "cdrom.h"
#include "cpu_state.h"
#include "debug_server.h"
#include "event_ring.h"
#include "psx_cycles.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

/* Event-timeline ring: execution-mode flag owned by dirty_ram_interp.c. The
 * static BIOS exception handler is NOT interp code, so we clear it around the
 * handler dispatch (see psx_check_interrupts). */
extern int g_dirty_interp_active;

/* Record an interrupt-delivery decision into the event ring. GATE outcomes are
 * edge-suppressed (they repeat every block while blocked); DELIVER is always
 * recorded (once per interrupt); the not-pending (idle) case emits nothing but
 * still updates the edge key so the next gate/deliver is captured. */
static void irq_record_outcome(uint8_t kind, uint8_t detail) {
    static uint16_t s_last = 0xFFFFu;
    uint16_t key = ((uint16_t)kind << 8) | detail;
    int repeat = (key == s_last);
    s_last = key;
    if (kind == EV_NONE) return;                 /* idle: update key only */
    if (kind == EV_IRQ_DELIVER) { event_ring_record(EV_IRQ_DELIVER, detail); return; }
    if (!repeat) event_ring_record(kind, detail);/* GATE: edge only */
}

/* COP0 register indices */
#define COP0_SR    12
#define COP0_CAUSE 13
#define COP0_EPC   14

/* I_STAT and I_MASK are owned by memory.c */
extern uint32_t i_stat;
extern uint32_t i_mask;

/* Dispatch counter for vblank scheduling. */
#define VBLANK_INTERVAL 50000        /* legacy: dispatch-count fallback (unused for VBlank gating now) */
#define VBLANK_CYCLES   564480u      /* 33.8688 MHz / 60 Hz — real PSX NTSC VBlank period */
static uint32_t dispatch_count;
static uint64_t total_checks;
static uint32_t cycles_since_vblank;  /* incremented by interrupts_advance_cycles */

void interrupts_advance_cycles(uint32_t cycles) {
    cycles_since_vblank += cycles;
}

/* Reentrancy guard: prevent interrupt handler from triggering interrupts. */
static int in_exception;

/* Diagnostic: total times the exception handler was entered (in_exception
 * transitioned 0->1).  Diff this between two snapshots to measure handler
 * dispatch rate. */
static uint64_t exception_entries_total;

/* Diagnostic: psx_check_interrupts called while already in_exception (the
 * `return` path at line ~159).  Real hardware can't double-fault into the
 * same handler; on our model this should stay near zero — non-zero means
 * something is calling psx_check_interrupts from inside the recompiled
 * exception handler tree. */
static uint64_t exception_reentry_blocks;

/* After the exception handler returns, suppress the next interrupt delivery
 * to give the interrupted code at least one block of execution — matching
 * real hardware where at least one instruction runs after RFE before the
 * pending interrupt can re-fire.  Without this, unhandled interrupts cause
 * a livelock: the handler runs, doesn't clear I_STAT, returns, and the
 * very next psx_check_interrupts re-enters immediately. */
static int post_exception_cooldown;

/* Blocks of guaranteed main-code forward progress imposed after a CLAIMED
 * non-SIO interrupt (DMA/VBLANK/timer/...). With cooldown 0 (the prior blanket
 * policy, commit 6d2cb65) the block leader at the interrupted PC re-takes the
 * exception before the block body executes, so under a fast-disc DMA flood the
 * main code is pinned at one PC forever (reentry-storm freeze). A few blocks
 * guarantee the interrupted block — and the field loop — advance between
 * deliveries. SIO is exempt (card reads need immediate back-to-back IRQs). */
#define CLAIMED_PROGRESS_QUANTUM 8

/* setjmp target for ReturnFromException during handler dispatch.
 *
 * IMPORTANT: longjmp(exception_jmpbuf, ...) MUST execute on the same
 * Windows fiber that called setjmp; otherwise RSP is restored to that
 * fiber's stack while the OS still tracks a different fiber as current,
 * corrupting fiber state and eventually deadlocking SwitchToFiber.
 *
 * s_exception_owner_fiber records which fiber called setjmp. If a
 * longjmp request originates on a different fiber, the caller must:
 *   (1) set s_pending_exception_longjmp = code
 *   (2) SwitchToFiber back to s_exception_owner_fiber
 * That fiber's wrapped SwitchToFiber call will observe the flag on
 * return and execute the longjmp on the correct stack. */
jmp_buf exception_jmpbuf;  /* non-static so traps.c can deferred-longjmp */
/* The fiber that owns the current exception setjmp. A longjmp must run on
 * that same fiber/stack, so a non-owner defers by switching back to it
 * first (see deferred_exception_longjmp). Used on all platforms now that
 * the thread scheduler is fiber-based everywhere. */
void* g_exception_owner_fiber = NULL;
int   g_pending_exception_longjmp = 0;
extern int g_psx_dispatch_depth;

int psx_get_in_exception(void) { return in_exception; }

void psx_get_freeze_diag(uint64_t *out_total_checks,
                         uint32_t *out_dispatch_count,
                         int *out_in_exception,
                         int *out_post_exc_cooldown,
                         uint64_t *out_exc_entries,
                         uint64_t *out_exc_reentry_blocks) {
    if (out_total_checks)        *out_total_checks       = total_checks;
    if (out_dispatch_count)      *out_dispatch_count     = dispatch_count;
    if (out_in_exception)        *out_in_exception       = in_exception;
    if (out_post_exc_cooldown)   *out_post_exc_cooldown  = post_exception_cooldown;
    if (out_exc_entries)         *out_exc_entries        = exception_entries_total;
    if (out_exc_reentry_blocks)  *out_exc_reentry_blocks = exception_reentry_blocks;
}

void interrupts_init(void) {
    dispatch_count = 0;
    in_exception = 0;
    g_psx_dispatch_depth = 0;
    total_checks = 0;
    post_exception_cooldown = 0;
    exception_entries_total = 0;
    exception_reentry_blocks = 0;
}

/*
 * Called by the runtime's ReturnFromException implementation (traps.c)
 * when the recompiled BIOS handler or a chain callback invokes
 * B0:0x17 or SYSCALL(3) during exception handling.
 *
 * At this point the caller has already:
 *   - Restored all GPRs from the TCB save area
 *   - Done RFE on the saved SR
 *   - Set cpu->pc = saved EPC
 *
 * We longjmp back to psx_check_interrupts, which will clear
 * in_exception and return, effectively letting the interrupted
 * code resume at the saved EPC through normal dispatch.
 */
#include "psx_fiber.h"
/* Defer a longjmp to the fiber that owns the current exception setjmp.
 * If we're on the owning fiber already, longjmp immediately. Otherwise
 * record the requested code, switch back to the owner, and the post-switch
 * check in traps.c psx_change_thread_fiber executes the longjmp on the
 * correct stack. */
static void deferred_exception_longjmp(int code) {
    if (!g_exception_owner_fiber || psx_fiber_current() == g_exception_owner_fiber) {
        longjmp(exception_jmpbuf, code);
    }
    g_pending_exception_longjmp = code;
    psx_fiber_switch(g_exception_owner_fiber);
    /* If we end up back here, the owner didn't honor the flag (bug).
     * Fall through to direct longjmp as a last resort — even though
     * the stack is wrong, the alternative is hanging silently. */
    longjmp(exception_jmpbuf, code);
}

void psx_exception_longjmp(void) {
    debug_server_log_restore_event(2, debug_cpu_ptr ? debug_cpu_ptr->pc : 0, 1);
    deferred_exception_longjmp(1);
}

void psx_restore_state_escape(void) {
    if (in_exception) {
        debug_server_log_restore_event(1, debug_cpu_ptr ? debug_cpu_ptr->pc : 0, 2);
        deferred_exception_longjmp(2);
    }
    /* Not in exception context — return normally, let caller's `return;` handle it. */
}

void psx_check_interrupts(CPUState* cpu) {
    total_checks++;
    if ((total_checks & 0x3FFFu) == 0) {
        debug_server_poll();
    }

    /* SIO delayed IRQ delivery removed from here.
     * sio_tick() is now called only from SIO register accesses
     * (sio_read/sio_write) and I_STAT reads (memory.c).  The BIOS
     * pad detection sequence clears I_STAT bit 7 then polls I_STAT
     * waiting for it to re-appear.  If we tick here, the IRQ fires
     * during the delay loop BEFORE the clear, and the BIOS never
     * sees it. */

    /* VBlank / timer tick — only when NOT inside the exception handler.
     *
     * On real hardware, VBlank is a hardware signal tied to the CRT
     * scanline counter, not to instruction count.  The handler runs
     * for a few hundred cycles at most.  In our model, the handler
     * runs thousands of block leaders (each calling psx_check_interrupts),
     * so dispatch_count can exceed VBLANK_INTERVAL during a single
     * handler invocation, causing VBlanks to stack up and starve the
     * main code.  Gating the tick on !in_exception prevents this. */
    /* Track SIO byte progress at every check (not just at VBlank time). */
    static uint32_t last_sio_seq_seen = 0;
    static uint64_t total_checks_at_progress = 0;
    {
        uint32_t cur_sio_seq = sio_get_seq();
        if (cur_sio_seq != last_sio_seq_seen) {
            last_sio_seq_seen = cur_sio_seq;
            total_checks_at_progress = total_checks;
        }
    }
    if (!in_exception) {
#if SIO_MODEL_CYCLE_PACED
        /* Builds without block-cycle accounting need a small dispatch-loop
         * SIO quantum. With PSX_ENABLE_BLOCK_CYCLES, psx_advance_cycles()
         * already drives SIO timing, so the fixed quantum would double-count.
         * arms shift/ack), so this gate never opens — per-call cost on
         * this hot path is one volatile load + one branch. */
#ifndef PSX_ENABLE_BLOCK_CYCLES
        if (g_sio_timing_active) {
            sio_tick_quantum();
        }
#endif
#endif
        dispatch_count++;
        /* VBlank pacing is now cycle-based (real PSX has VBlank at
         * VBLANK_CYCLES = 564480 = 33.8688 MHz / 60 Hz). Previously this
         * used dispatch_count >= 50000 which fired at ~5-6 cycles per
         * dispatch = ~300k cycles per VBlank — half of real PSX, so
         * guest time ran at ~60% real rate. That broke FMV pacing
         * (TombaRecomp ISSUES.md #4: FMV at 6.7 fps vs 15 fps target). */
        if (cycles_since_vblank >= VBLANK_CYCLES) {
            /* Defer VBlank while a card SIO transaction is mid-flight.
             * On real hardware a 140-byte sector read finishes (~4.5ms)
             * well before the next VBlank (16.67ms).  Our dispatch-count
             * pacing fires VBlank during the read window, and the BIOS
             * VBlank pad poll injects 0x01 onto the SIO bus mid-card
             * read, aborting the transaction via CTRL.RESET.
             *
             * Defer as long as the card protocol is making progress
             * (an SIO byte was exchanged in the last VBLANK_DEFER_STALE
             * dispatch ticks).  If no byte for that long, treat the
             * protocol as stuck and force the VBlank to prevent deadlock. */
            #define VBLANK_DEFER_STALE 500000  /* ~10 frames; covers inter-byte gaps in card protocol */
            int card_active = sio_card_protocol_active();
            uint64_t since_progress = total_checks - total_checks_at_progress;
            int progress_stale = since_progress >= VBLANK_DEFER_STALE;
            if (!card_active || progress_stale) {
                /* Subtract one VBlank period rather than reset to 0 so
                 * cycle overshoot carries forward. Prevents long-running
                 * blocks from rounding multiple VBlanks together. */
                cycles_since_vblank -= VBLANK_CYCLES;
                dispatch_count = 0;
                /* DEQUEUE: this VBlank fired. ENQUEUE: next VBlank scheduled
                 * one period out. */
                event_ring_record_aux(EV_DEQ, (uint8_t)SRC_VBLANK,
                                      (uint32_t)psx_get_cycle_count());
                event_ring_record_aux(EV_ENQ, (uint8_t)SRC_VBLANK,
                                      (uint32_t)(psx_get_cycle_count() + VBLANK_CYCLES));
                i_stat |= (1 << IRQ_VBLANK);
                event_ring_record(EV_ISTAT_RAISE, IRQ_VBLANK);
                gpu_vblank_tick();  /* Toggle LCF (GPUSTAT bit 31) */
#ifndef PSX_ENABLE_BLOCK_CYCLES
                timers_tick(33868); /* ~1 NTSC frame worth of cycles */
                cdrom_tick();      /* Process pending CDROM responses */
#endif
            }
        }
    }

    /* Event ring: generic i_stat-edge backstop. Catches raises from sites we
     * don't instrument precisely (memory.c MMIO acks, SIO, SPU). Bounded by
     * actual transitions, not by check frequency. */
    {
        static uint32_t s_last_istat = 0;
        if (i_stat != s_last_istat) {
            s_last_istat = i_stat;
            event_ring_record(EV_ISTAT_CHANGE, 0);
        }
    }

    /* Check if any interrupts are pending. */
    if ((i_stat & i_mask) == 0) { irq_record_outcome(EV_NONE, 0); return; }
    if (in_exception) {
        exception_reentry_blocks++;
        irq_record_outcome(EV_IRQ_GATE, GATE_IN_EXCEPTION);
        return;
    }

    /* Post-exception cooldown: let at least one block execute after RFE. */
    if (post_exception_cooldown > 0) {
        post_exception_cooldown--;
        irq_record_outcome(EV_IRQ_GATE, GATE_COOLDOWN);
        return;
    }

    /* Check COP0 SR: IEc (bit 0) must be set, and IM2 (bit 10) must be set. */
    uint32_t sr = cpu->cop0[COP0_SR];
    if (!(sr & 0x01)) { irq_record_outcome(EV_IRQ_GATE, GATE_SR_IE); return; }   /* Interrupts globally disabled */
    if (!(sr & (1 << 10))) { irq_record_outcome(EV_IRQ_GATE, GATE_SR_IM2); return; } /* Hardware interrupt bit not enabled */

    irq_record_outcome(EV_IRQ_DELIVER, 0);
    in_exception = 1;
    exception_entries_total++;
    uint32_t pre_handler_istat = i_stat;  /* snapshot for cooldown decision */

    /* Set COP0 Cause: ExcCode=0 (interrupt), IP2 pending. */
    cpu->cop0[COP0_CAUSE] = (cpu->cop0[COP0_CAUSE] & ~0x7C) | (0 << 2);
    cpu->cop0[COP0_CAUSE] |= (1 << 10);

    /* Push SR exception stack: shift bits [5:0] left by 2. */
    cpu->cop0[COP0_SR] = (sr & ~0x3F) | ((sr & 0x0F) << 2);

    /* EPC: set to a sentinel value. The recompiled exception handler reads
     * memory at [EPC] to check for COP2 branch delay. We use a dedicated
     * address in the kernel scratch area.  Address 0x80000048 is chosen
     * because it's between the exception vectors (0x80-0xBF) and the
     * kernel data pointer area (0x100+), and not used by the BIOS. */
    uint32_t sentinel = 0x80000048u;
    cpu->write_word(sentinel, 0x00000000u); /* NOP */
    cpu->cop0[COP0_EPC] = sentinel;

    /* Save the interrupted code's full register state.
     *
     * On real hardware, the exception handler saves all GPRs to the
     * TCB save area at entry, and ReturnFromException restores them
     * before jumping back to EPC.  The interrupted code always gets
     * its exact pre-exception register values back.
     *
     * In our model, the recompiled handler runs as a C function and
     * its `return;` goes back here — not to EPC.  The handler's
     * normal exit path (0xBFC10944) restores registers from the
     * kernel jmpbuf (intended for longjmp to WaitEvent's caller),
     * which corrupts the interrupted code's registers.
     *
     * We save all GPRs/HI/LO before the handler and restore them
     * after, so the interrupted code resumes with its original state
     * — matching real hardware behaviour. */
    uint32_t saved_gpr[32];
    uint32_t saved_hi, saved_lo;
    for (int i = 0; i < 32; i++) saved_gpr[i] = cpu->gpr[i];
    saved_hi = cpu->hi;
    saved_lo = cpu->lo;

    /* Dispatch the BIOS exception handler.
     * BEV (SR bit 22) selects between 0x80000080 and 0xBFC00180.
     *
     * setjmp is placed here so ReturnFromException (longjmp code 1)
     * and RestoreState (longjmp code 2) can escape the handler call
     * tree.
     *
     * The loop handles the PSX VSync mechanism (SaveState/RestoreState):
     *   - Code 0: normal entry — dispatch the handler.
     *   - Code 2: RestoreState redirect — re-dispatch to cpu->pc
     *     (e.g. VSync callback loop at 0xBFC421D8), still in exception
     *     context.  The redirected code eventually calls ReturnFromException.
     *   - Code 1: ReturnFromException — exit the loop entirely. */
    uint32_t target_pc;
    if (sr & 0x00400000u) {
        target_pc = 0xBFC00180u;
    } else {
        uint32_t w0 = cpu->read_word(0x80000080u);
        uint32_t w1 = cpu->read_word(0x80000084u);
        uint32_t hi_val = (w0 & 0xFFFF) << 16;
        int16_t lo_val = (int16_t)(w1 & 0xFFFF);
        target_pc = hi_val + (uint32_t)(int32_t)lo_val;
    }

    /* Record which fiber owns this setjmp. Any subsequent longjmp must
     * happen on this same fiber; if a non-owner fiber needs to longjmp
     * it must switch back here first (see deferred_exception_longjmp). */
    void *prev_owner_fiber = g_exception_owner_fiber;
    int   prev_pending = g_pending_exception_longjmp;
    g_exception_owner_fiber = psx_fiber_current();
    g_pending_exception_longjmp = 0;
    /* A bail unwind can never be in flight at exception entry: bail-mode
     * returns skip every block leader, so psx_check_interrupts is never
     * reached while g_psx_call_bail is set.  If it ever is, count the
     * anomaly and clear so the handler dispatch isn't poisoned. */
    if (g_psx_call_bail) {
        g_psx_bail_anomaly++;
        g_psx_call_bail = 0;
    }
    /* The static BIOS exception handler is not dirty-RAM-interp code. Clear the
     * interp mode flag across the handler dispatch so events recorded inside it
     * are tagged STATIC. The restore sits after the loop the longjmp lands in,
     * so the EPC-sentinel longjmp can't leave the flag wrong. */
    int prev_interp_active = g_dirty_interp_active;
    g_dirty_interp_active = 0;
    for (;;) {
        int jmp_val = setjmp(exception_jmpbuf);
        if (jmp_val == 2) {
            /* RestoreState redirect: re-dispatch to cpu->pc.
             * GPRs were already set by RestoreState — do NOT restore.
             * Stay in exception context so ReturnFromException works. */
            g_psx_dispatch_depth = 0;
            debug_server_log_restore_event(3, cpu->pc, (uint32_t)jmp_val);
            target_pc = cpu->pc;
            continue;
        }
        if (jmp_val == 1) {
            g_psx_dispatch_depth = 0;
            debug_server_log_restore_event(4, cpu->pc, (uint32_t)jmp_val);
        }
        if (jmp_val == 0) {
            /* Normal entry (or after RestoreState redirect): dispatch. */
            psx_dispatch(cpu, target_pc);
        }
        /* jmp_val 0 (normal return) or 1 (ReturnFromException): done. */
        break;
    }
    /* Restore previous exception-owner state. Supports nested exceptions
     * if they ever arise (uncommon but harmless). */
    g_exception_owner_fiber = prev_owner_fiber;
    g_pending_exception_longjmp = prev_pending;
    g_dirty_interp_active = prev_interp_active;

    /* Restore the interrupted code's registers.
     *
     * The handler has done its work (acknowledged i_stat, delivered
     * events, etc.) via MMIO writes and RAM writes — those side
     * effects are in memory.  We restore GPRs so the interrupted
     * code continues with its pre-exception state.
     *
     * For SR: if the handler did ReturnFromException (RFE already
     * applied, IEc restored), we keep that.  If the handler exited
     * normally (jmpbuf path, no RFE), IEc is still 0 from the
     * exception push — we do RFE to pop the SR stack. */
    for (int i = 0; i < 32; i++) cpu->gpr[i] = saved_gpr[i];
    cpu->hi = saved_hi;
    cpu->lo = saved_lo;

    if (!(cpu->cop0[COP0_SR] & 0x01)) {
        uint32_t sr2 = cpu->cop0[COP0_SR];
        cpu->cop0[COP0_SR] = (sr2 & 0xFFFFFFC0u) | ((sr2 >> 2) & 0x0Fu);
    }

    in_exception = 0;

    /* Adaptive cooldown: if the handler acknowledged the interrupt (cleared
     * some I_STAT bits), the interrupt won't immediately re-fire and we need
     * no cooldown.  If I_STAT is unchanged (no handler claimed the interrupt),
     * give the main code a generous window to make progress — e.g. to let
     * the shell finish installing handlers.  On real hardware, the CPU
     * executes at least one instruction between exceptions; in our model
     * each "block" is many instructions, but the handler also consumes
     * hundreds of sub-dispatches per invocation. */
    if ((i_stat & i_mask) != 0 && i_stat == pre_handler_istat) {
        post_exception_cooldown = 500;  /* unclaimed: give main code time */
    } else {
        /* Claimed: the handler acknowledged at least one I_STAT bit. Per-source
         * policy (this used to be a blanket cooldown=0 — commit 6d2cb65 — which
         * pins main code under a fast-disc DMA flood: the interrupted block's
         * leader re-takes the exception before its body runs):
         *   - SIO (card reads): 128 consecutive SIO IRQs must fire within one
         *     blocking wait; any gap stalls the card protocol → re-fire now.
         *   - DMA/VBLANK/timer/etc: guarantee a few blocks of main-code
         *     progress between deliveries so a flood can't starve the loop. */
        uint32_t claimed = pre_handler_istat & ~i_stat;            /* bits handler cleared */
        uint32_t sio_active = (claimed | (i_stat & i_mask)) & (1u << IRQ_SIO0);
        post_exception_cooldown = sio_active ? 0 : CLAIMED_PROGRESS_QUANTUM;
    }
}

/* Compatibility shim: the ape-flavored generated code calls
 * psx_check_interrupts_at(cpu, resume_pc); the mmx6-fw baseline runtime delivers
 * interrupts via psx_check_interrupts (cpu->pc / scratch sentinel). Forwarding
 * here gives the mmx6 baseline interrupt behavior — sufficient to build+run the
 * current generated code on the good baseline for instrumented comparison. */
void psx_check_interrupts_at(CPUState* cpu, uint32_t resume_pc) {
    (void)resume_pc;
    psx_check_interrupts(cpu);
}

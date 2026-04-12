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
 */

#include "interrupts.h"
#include "timers.h"
#include "cpu_state.h"
#include <stdio.h>
#include <stdlib.h>

/* COP0 register indices */
#define COP0_SR    12
#define COP0_CAUSE 13
#define COP0_EPC   14

/* I_STAT and I_MASK are owned by memory.c */
extern uint32_t i_stat;
extern uint32_t i_mask;

/* Dispatch counter for vblank scheduling. */
#define VBLANK_INTERVAL 50000
static uint32_t dispatch_count;
static uint64_t total_checks;

/* Reentrancy guard: prevent interrupt handler from triggering interrupts. */
static int in_exception;

void interrupts_init(void) {
    dispatch_count = 0;
    in_exception = 0;
    total_checks = 0;
}

void psx_check_interrupts(CPUState* cpu) {
    /* Periodic vblank + timer tick. */
    dispatch_count++;
    total_checks++;

    /* Dump diagnostic state periodically. */
    if (total_checks % 1000000 == 0) {
        FILE* df = fopen("psx_diag.txt", "a");
        if (df) {
            uint32_t sr = cpu->cop0[COP0_SR];
            fprintf(df, "total_checks=%llu dispatch_count=%u\n",
                    (unsigned long long)total_checks, dispatch_count);
            fprintf(df, "i_stat=0x%08X i_mask=0x%08X pending=0x%08X\n",
                    i_stat, i_mask, i_stat & i_mask);
            fprintf(df, "COP0_SR=0x%08X (IEc=%d IM2=%d BEV=%d)\n",
                    sr, sr & 1, (sr >> 10) & 1, (sr >> 22) & 1);
            fprintf(df, "COP0_CAUSE=0x%08X COP0_EPC=0x%08X\n",
                    cpu->cop0[COP0_CAUSE], cpu->cop0[COP0_EPC]);
            fprintf(df, "in_exception=%d\n", in_exception);
            fprintf(df, "gpr[31](ra)=0x%08X gpr[29](sp)=0x%08X\n",
                    cpu->gpr[31], cpu->gpr[29]);
            /* Check if exception handler is installed at 0x80000080 */
            uint32_t handler_word = cpu->read_word(0x80000080u);
            fprintf(df, "handler@80000080=0x%08X\n", handler_word);
            fclose(df);
        }
    }

    if (dispatch_count >= VBLANK_INTERVAL) {
        dispatch_count = 0;
        i_stat |= (1 << IRQ_VBLANK);
        timers_tick(33868); /* ~1 NTSC frame worth of cycles */
    }

    /* Check if any interrupts are pending. */
    if ((i_stat & i_mask) == 0) return;
    if (in_exception) return;

    /* Check COP0 SR: IEc (bit 0) must be set, and IM2 (bit 10) must be set. */
    uint32_t sr = cpu->cop0[COP0_SR];
    if (!(sr & 0x01)) return;    /* Interrupts globally disabled */
    if (!(sr & (1 << 10))) return; /* Hardware interrupt bit not enabled */

    in_exception = 1;

    /* Set COP0 Cause: ExcCode=0 (interrupt), IP2 pending. */
    cpu->cop0[COP0_CAUSE] = (cpu->cop0[COP0_CAUSE] & ~0x7C) | (0 << 2);
    cpu->cop0[COP0_CAUSE] |= (1 << 10);

    /* Push SR exception stack: shift bits [5:0] left by 2. */
    cpu->cop0[COP0_SR] = (sr & ~0x3F) | ((sr & 0x0F) << 2);

    /* EPC: set to a sentinel value. The recompiled exception handler reads
     * memory at [EPC] to check for COP2 branch delay. We use a RAM address
     * that we write a NOP to. */
    uint32_t sentinel = 0x80000040u;
    cpu->write_word(sentinel, 0x00000000u); /* NOP */
    cpu->cop0[COP0_EPC] = sentinel;

    /* Dispatch the BIOS exception handler at 0x80000080.
     * BEV (SR bit 22) selects between 0x80000080 and 0xBFC00180. */
    uint32_t vector = (sr & 0x00400000u) ? 0xBFC00180u : 0x80000080u;
    psx_dispatch(cpu, vector);

    in_exception = 0;
}

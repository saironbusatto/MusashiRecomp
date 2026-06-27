/* psx_cycles.c — PSX guest CPU cycle clock. */

#include "psx_cycles.h"
#include "cpu_state.h"
#include "cdrom.h"
#include "dma.h"
#include "interrupts.h"
#include "sio.h"
#include "starvation_ring.h"
#include "timers.h"

uint64_t psx_cycle_count = 0;

/* Throttle watchdog check to once per ~64K cycles to keep hot-path cost
 * negligible (most blocks emit 5-30 cycles, so the check fires every
 * ~2K-12K blocks). */
static uint32_t s_watchdog_throttle = 0;
static uint32_t s_pc_sample_throttle = 0;

/* Conservative event-granularity diagnostic (set via debug cmd
 * overlay_native_event_granularity). Normally psx_advance_cycles charges a
 * whole basic block's cycles in ONE step, so every device advances N cycles at
 * once and any events that came due at sub-block cycles all fire together, in
 * the fixed device order below (sio,cdrom,dma,timers,interrupts) — NOT in true
 * due-cycle order. The dirty-RAM interpreter avoids this only because it calls
 * us with N=1 per instruction. When this flag is set, a batched (N>1) advance
 * is split into N single-cycle steps, so device events fire at their true
 * due-cycle in order — i.e. native execution gets the same event timeline the
 * interpreter produces. Diagnostic: if the village->overworld blue screen
 * clears with this on, the root cause is per-block event-ordering, and the
 * real fix is a due-cycle event scheduler (run-to-next-event), not this. */
int g_event_step_conservative = 0;

static void advance_devices(uint32_t c) {
    psx_cycle_count += (uint64_t)c;
    sio_advance(c);
    cdrom_advance(c);
    dma_advance(c);
    timers_advance(c);
    interrupts_advance_cycles(c);
}

void psx_advance_cycles(uint32_t cycles) {
    if (cycles == 0) return;
    if (g_event_step_conservative && cycles > 1u) {
        /* Fine-step so sub-block events fire in true cycle order. */
        for (uint32_t i = 0; i < cycles; i++) advance_devices(1u);
    } else {
        advance_devices(cycles);
    }
    s_watchdog_throttle += cycles;
    if (s_watchdog_throttle >= 65536u) {
        s_watchdog_throttle = 0;
        starvation_watchdog_check();
    }
    /* PC sample every ~1M cycles (~30us PSX, ~333Hz) — small enough to
     * localize a busy-wait loop, sparse enough to not flood the 16K ring
     * during normal SIO traffic (~3000 samples/sec vs >10K SIO events/sec
     * during card transactions). */
    s_pc_sample_throttle += cycles;
    if (s_pc_sample_throttle >= 1048576u) {
        s_pc_sample_throttle = 0;
        starvation_ring_pc_sample();
    }
}

uint64_t psx_get_cycle_count(void) {
    return psx_cycle_count;
}

/* ---- Mult/div completion-stall timing (faithful R3000A; Beetle muldiv_ts_done) ----
 *
 * MULT/MULTU/DIV/DIVU don't stall at the op; they set a completion DEADLINE.
 * A later MFLO/MFHI that reads HI/LO before the deadline STALLS (advances guest
 * cycles) until it. Instructions executed in between absorb the latency — so the
 * stall is (deadline - now), not a flat charge (this is why div+2filler+mflo costs
 * the same as div+mflo: the fillers ran during the latency window). REQUIRES
 * per-instruction cycle charging (PSX_CODEGEN_CYCLE_PER_INSN / the interp), so
 * `now` is the true cycle position at the op — block-up-front charging breaks it.
 *
 * Latencies transcribed from Beetle cpu.cpp: DIV/DIVU = 37 (fixed). MULT/MULTU =
 * MULT_Tab24 indexed by the leading-zero count of the (sign-folded, for signed)
 * first operand | 0x400 — i.e. 14 for small magnitudes (<12 significant bits),
 * 10 for medium, 7 for large. The | 0x400 caps the index at 21 (never l==0). */

static const uint8_t PSX_MULT_TAB24[24] = {
    /* i<12: 7+4+3=14 */ 14,14,14,14,14,14,14,14,14,14,14,14,
    /* 12<=i<21: 7+3=10 */ 10,10,10,10,10,10,10,10,10,
    /* i>=21: 7 */ 7,7,7
};

static inline uint32_t psx_clz32(uint32_t v) {
    /* v is never 0 here (callers OR in 0x400). */
    return (uint32_t)__builtin_clz(v);
}

uint32_t psx_mult_latency_s(uint32_t rs) {  /* MULT (signed): sign-fold magnitude */
    return PSX_MULT_TAB24[psx_clz32((rs ^ (uint32_t)((int32_t)rs >> 31)) | 0x400u)];
}
uint32_t psx_mult_latency_u(uint32_t rs) {  /* MULTU (unsigned) */
    return PSX_MULT_TAB24[psx_clz32(rs | 0x400u)];
}

/* DIV/DIVU latency is the fixed constant 37 — emitted directly at the op site. */

void psx_muldiv_set(CPUState* cpu, uint32_t latency) {
    cpu->muldiv_ts_done = psx_cycle_count + (uint64_t)latency;
}

void psx_muldiv_stall(CPUState* cpu) {
    if (cpu->muldiv_ts_done > psx_cycle_count) {
        psx_advance_cycles((uint32_t)(cpu->muldiv_ts_done - psx_cycle_count));
    }
}

/* ---- GTE (COP2) per-command completion-stall timing ----
 *
 * Faithful R3000A/GTE model (Beetle cpu.cpp:1410-1412 + gte.cpp GTE_Instruction
 * return(ret-1)). Each GTE command takes `cost` cycles (gte.cpp per-op returns,
 * verified from source); the COP2 instruction's own +1 base is charged
 * separately by per-instruction charging, so the *added* deadline latency is
 * cost-1. A later COP2 register access (MFC2/CFC2/MTC2/CTC2/LWC2/SWC2) stalls
 * until the deadline. Back-to-back commands serialize: psx_gte_set stalls to the
 * prior deadline before arming the next (Beetle stalls timestamp to gte_ts_done
 * at the command site before computing the new gte_ts_done).
 *
 * Cost table = (cost-1), indexed by the 6-bit GTE command (instr & 0x3F).
 * cost values transcribed + verified from beetle-psx/mednafen/psx/gte.cpp op
 * returns: RTPS15 RTPT23 MVMVA8 SQR5 OP6 AVSZ3/4=5 NCLIP8 NCDS19 NCDT44 NCCS17
 * NCCT39 NCS14 NCT30 CC11 CDP13 DPCS8 DPCT17 DCPL8 INTPL8 GPF5 GPL5. Unknown/
 * undefined commands = 1 cycle (Beetle default ret=1) -> 0 added. */
static const uint8_t PSX_GTE_LAT_M1[64] = {
    [0x00] = 14, [0x01] = 14,            /* RTPS  */
    [0x06] = 7,                          /* NCLIP */
    [0x0C] = 5,                          /* OP    */
    [0x10] = 7,                          /* DPCS  */
    [0x11] = 7,                          /* INTPL */
    [0x12] = 7,                          /* MVMVA */
    [0x13] = 18,                         /* NCDS  */
    [0x14] = 12,                         /* CDP   */
    [0x16] = 43,                         /* NCDT  */
    [0x1A] = 7,                          /* DCPL (alt of 0x29) */
    [0x1B] = 16,                         /* NCCS  */
    [0x1C] = 10,                         /* CC    */
    [0x1E] = 13,                         /* NCS   */
    [0x20] = 29,                         /* NCT   */
    [0x28] = 4,                          /* SQR   */
    [0x29] = 7,                          /* DCPL  */
    [0x2A] = 16,                         /* DPCT  */
    [0x2D] = 4,                          /* AVSZ3 */
    [0x2E] = 4,                          /* AVSZ4 */
    [0x30] = 22,                         /* RTPT  */
    [0x3D] = 4,                          /* GPF   */
    [0x3E] = 4,                          /* GPL   */
    [0x3F] = 38,                         /* NCCT  */
};

uint32_t psx_gte_cmd_latency(uint32_t cmd) {
    return (uint32_t)PSX_GTE_LAT_M1[cmd & 0x3Fu];
}

void psx_gte_set(CPUState* cpu, uint32_t latency) {
    /* Back-to-back GTE ops serialize: finish the prior op first. */
    if (cpu->gte_ts_done > psx_cycle_count) {
        psx_advance_cycles((uint32_t)(cpu->gte_ts_done - psx_cycle_count));
    }
    cpu->gte_ts_done = psx_cycle_count + (uint64_t)latency;
}

void psx_gte_stall(CPUState* cpu) {
    if (cpu->gte_ts_done > psx_cycle_count) {
        psx_advance_cycles((uint32_t)(cpu->gte_ts_done - psx_cycle_count));
    }
}

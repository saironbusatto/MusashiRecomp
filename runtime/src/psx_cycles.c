/* psx_cycles.c — PSX guest CPU cycle clock. */

#include "psx_cycles.h"
#include "cpu_state.h"
#include "cdrom.h"
#include "dma.h"
#include "interrupts.h"
#include "sio.h"
#include "starvation_ring.h"
#include "timers.h"
#ifdef PSX_COSIM
#include "cosim_state.h"
#endif

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

/* ===== Event-deadline device servicing (production fast path) =================
 *
 * The faithful core charges guest cycles PER INSTRUCTION (psx_cyc.h §1), and the
 * legacy path below walked EVERY device on EVERY charge — ~10 host calls per
 * guest instruction, ~300M calls/s: the whole prod build pegged a core inside
 * psx_advance_cycles (gdb-sampled 14/14, leaves = timers/cdrom/dma/interrupts).
 *
 * FAITHFUL_TIMING_PLAN.md's target architecture is the fix: cycles accumulate
 * into the ONE global counter cheaply; devices are only SERVICED when the next
 * event deadline is reached (cycles_to_next_event(): min over vblank / timer /
 * cdrom / dma / sio IRQ distances), or when guest code touches device MMIO
 * (memory.c calls psx_devices_mmio_sync() so reads see current state and writes
 * that re-arm a device force a deadline recompute).
 *
 * Guest-visible semantics are IDENTICAL: servicing rewinds psx_cycle_count to
 * the devices' synced position and re-plays the gap through the SAME
 * vblank-bounded chunk loop the legacy path used, so device code observes the
 * exact same cycle values it always did, and no event can fire late because the
 * deadline is by construction <= the earliest device event (hard-capped at
 * DEADLINE_HARD_CAP for IRQ-masked device progress + frame pacing).
 *
 * The exact per-charge path is kept verbatim for PSX_COSIM builds (oracle
 * determinism / checkpoint alignment) and for the g_event_step_conservative
 * diagnostic toggle. */
#define PSX_DEADLINE_HARD_CAP 16384u

static uint64_t s_devices_synced_cycle = 0;  /* devices are advanced up to here */
static uint64_t s_next_service_cycle   = 0;  /* absolute; 0 = dirty, recompute  */
static int      s_in_device_service    = 0;  /* re-entrancy guard               */

/* Distance to the nearest INTERNAL device event, mask-blind (i_mask =
 * all-unmasked). This is the chunking bound for catch-up: the sio/cdrom/dma
 * *_advance() functions fire at most ONE event boundary per call (their
 * legacy caller stepped 1 cycle at a time), so every catch-up chunk must land
 * exactly ON the nearest event so chained sequences (SIO byte -> ack -> next
 * byte; CD sector trains) replay event-by-event, and so events armed while
 * their IRQ is masked in I_MASK still process on time (games poll I_STAT). */
static uint32_t devices_cycles_to_next_internal_event(void) {
    uint32_t best = interrupts_cycles_to_vblank();   /* frame pacing always */
    uint32_t t = timers_cycles_to_irq(0xFFFFFFFFu);  if (t < best) best = t;
    uint32_t c = cdrom_cycles_to_irq(0xFFFFFFFFu);   if (c < best) best = c;
    uint32_t d = dma_cycles_to_irq(0xFFFFFFFFu);     if (d < best) best = d;
    uint32_t s = sio_cycles_to_irq(0xFFFFFFFFu);     if (s < best) best = s;
    if (best == 0) best = 1;    /* due/overdue: process within one cycle */
    return best;
}

static void psx_devices_recompute_deadline(void) {
    uint32_t next = devices_cycles_to_next_internal_event();
    if (next > PSX_DEADLINE_HARD_CAP) next = PSX_DEADLINE_HARD_CAP;
    s_next_service_cycle = psx_cycle_count + (uint64_t)next;
}

static void psx_devices_service_to_now(void) {
    if (s_in_device_service) return;                 /* device code charged cycles: absorb */
    s_in_device_service = 1;
    uint64_t target = psx_cycle_count;
    if (s_devices_synced_cycle < target) {
        /* Rewind and re-play the gap in event-bounded chunks so device
         * callbacks see the same incremental psx_cycle_count they always did
         * and no chunk ever skips OVER a device event boundary. */
        psx_cycle_count = s_devices_synced_cycle;
        interrupts_service_scheduled_events();
        while (psx_cycle_count < target) {
            uint32_t remaining = (uint32_t)(target - psx_cycle_count);
            uint32_t step = remaining;
            uint32_t to_ev = devices_cycles_to_next_internal_event();
            if (to_ev > 0 && to_ev < step) step = to_ev;
            if (step == 0) step = 1;
            advance_devices(step);
            interrupts_service_scheduled_events();
        }
        s_devices_synced_cycle = target;
    }
    psx_devices_recompute_deadline();
    s_in_device_service = 0;
}

/* memory.c hook: called at the top of every device-MMIO read/write wrapper.
 * Catch devices up so the handler sees current state, and mark the deadline
 * dirty so a write that re-arms a device (timer target, CD command, DMA kick)
 * can only shorten — never silently extend — the next service. */
void psx_devices_mmio_sync(void) {
    if (s_devices_synced_cycle != psx_cycle_count) {
        psx_devices_service_to_now();
    }
    s_next_service_cycle = 0;   /* recompute on the next charge */
}

/* Exact per-charge path (legacy semantics). Used by PSX_COSIM builds and the
 * g_event_step_conservative diagnostic; also keeps the deadline-path state
 * coherent so the two can interleave (the runtime toggle flips mid-run). */
static void psx_advance_cycles_exact(uint32_t cycles) {
#ifdef PSX_COSIM
    /* First-divergence oracle: the guest-cycle counter is the ONLY clock both backends
     * share identically, so it is the alignment point for the full-state hash. */
    cosim_tick();
#endif
    interrupts_service_scheduled_events();
    while (cycles > 0) {
        uint32_t step = cycles;
        if (g_event_step_conservative && step > 1u) {
            step = 1u;
        } else {
            uint32_t to_vblank = interrupts_cycles_to_vblank();
            if (to_vblank > 0 && to_vblank < step) step = to_vblank;
#ifdef PSX_COSIM
            uint32_t to_cp = cosim_cycles_to_next_checkpoint();
            if (to_cp > 0 && to_cp < step) step = to_cp;
#endif
        }
        if (step == 0) step = cycles;
        advance_devices(step);
        cycles -= step;
        interrupts_service_scheduled_events();
#ifdef PSX_COSIM
        cosim_tick();
#endif
    }
    s_devices_synced_cycle = psx_cycle_count;
    s_next_service_cycle   = 0;
}

void psx_advance_cycles(uint32_t cycles) {
    { extern int g_ls_replay_active; if (g_ls_replay_active) return; }  /* lockstep replay: no global cycle/device mutation */
    if (cycles == 0) return;
    uint32_t charged_cycles = cycles;
#ifdef PSX_COSIM
    psx_advance_cycles_exact(cycles);
#else
    if (g_event_step_conservative) {
        psx_advance_cycles_exact(cycles);
    } else if (s_in_device_service) {
        /* Charge from inside device servicing (defensive): count it, service later. */
        psx_cycle_count += (uint64_t)cycles;
    } else {
        psx_cycle_count += (uint64_t)cycles;
        if (s_next_service_cycle == 0 || psx_cycle_count >= s_next_service_cycle) {
            psx_devices_service_to_now();
        }
    }
#endif
    s_watchdog_throttle += charged_cycles;
    if (s_watchdog_throttle >= 65536u) {
        s_watchdog_throttle = 0;
        starvation_watchdog_check();
    }
    /* PC sample every ~1M cycles (~30us PSX, ~333Hz) — small enough to
     * localize a busy-wait loop, sparse enough to not flood the 16K ring
     * during normal SIO traffic (~3000 samples/sec vs >10K SIO events/sec
     * during card transactions). */
    s_pc_sample_throttle += charged_cycles;
    if (s_pc_sample_throttle >= 1048576u) {
        s_pc_sample_throttle = 0;
        starvation_ring_pc_sample();
    }
}

uint64_t psx_get_cycle_count(void) {
    return psx_cycle_count;
}

/* Save-state restore: psx_cycle_count was just overwritten from the snapshot, so
 * the deadline-model bookkeeping (synced position + next deadline) is stale and
 * would try to replay a bogus gap. Re-anchor devices at the restored cycle and
 * force a fresh deadline on the next charge. */
void psx_cycles_resync_after_restore(void) {
    s_devices_synced_cycle = psx_cycle_count;
    s_next_service_cycle   = 0;   /* recompute on next charge */
    s_in_device_service    = 0;
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
    /* MFLO/MFHI stall to the mult/div completion deadline (Beetle cpu.cpp:1723-1736).
     * While stalling it CONSUMES a pending load-delay give-back (read_absorb) — each
     * stalled cycle decrements read_absorb[read_absorb_which] — so cycles that would
     * have been "free" for following instructions are spent here instead. Plus the
     * off-by-one shortcut: a deadline exactly one cycle out just retracts (no stall).
     * (No-load code has read_absorb==0, so this reduces to a plain advance.) */
    if (cpu->muldiv_ts_done > psx_cycle_count) {
        if (cpu->muldiv_ts_done == psx_cycle_count + 1u) {
            cpu->muldiv_ts_done--;   /* off-by-one: retract the deadline, no advance */
            return;
        }
        uint32_t stall = (uint32_t)(cpu->muldiv_ts_done - psx_cycle_count);
        uint8_t w = cpu->read_absorb_which;          /* fixed during the stall */
        uint32_t give = cpu->read_absorb[w];
        cpu->read_absorb[w] = (uint8_t)(give > stall ? give - stall : 0u);  /* consume */
        psx_advance_cycles(stall);
    }
}

/* MFC2/CFC2 (GTE register read → GPR): stall to the GTE command completion deadline
 * AND hand the stall amount to the next instruction(s) as a load-delay give-back
 * (Beetle cpu.cpp:1332-1341: LDAbsorb = gte_ts_done - timestamp, LDWhich = rt). The
 * §1+DO_LDS that bracket this ran in the instruction's psx_cyc_step (COP2 is non-load).
 * MTC2/CTC2 (writes) use psx_gte_stall (stall only, no give-back). */
void psx_gte_read(CPUState* cpu, uint32_t rt) {
    if (cpu->gte_ts_done > psx_cycle_count) {
        uint32_t stall = (uint32_t)(cpu->gte_ts_done - psx_cycle_count);
        cpu->ld_absorb = stall;
        psx_advance_cycles(stall);
    } else {
        cpu->ld_absorb = 0u;
    }
    cpu->ld_which_t = (uint8_t)rt;
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

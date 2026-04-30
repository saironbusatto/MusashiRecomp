/* psx_cycles.c — PSX guest CPU cycle clock.
 *
 * Phase 1.0e-d minimal slice: receives per-block cycle counts from
 * generated code (when compiled with -DPSX_ENABLE_BLOCK_CYCLES=1) and
 * accumulates them in psx_cycle_count. NO peripheral hooks — wiring
 * sio_tick here would corrupt the legacy IRQ countdown (it decrements
 * unconditionally per call; millions of calls per second drain it in
 * microseconds). A future slice with a peripheral-only sio_advance
 * (no legacy countdown side effect) will wire SIO. */

#include "psx_cycles.h"

uint64_t psx_cycle_count = 0;

void psx_advance_cycles(uint32_t cycles) {
    if (cycles == 0) return;
    psx_cycle_count += (uint64_t)cycles;
    /* No peripheral dispatch in this slice — see comment above. */
}

uint64_t psx_get_cycle_count(void) {
    return psx_cycle_count;
}

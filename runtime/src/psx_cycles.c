/* psx_cycles.c — PSX guest CPU cycle clock.
 *
 * Phase 1.0e-e1: dispatches to peripheral-only sio_advance (does NOT
 * touch legacy sio_irq_pending/countdown). Until 1.0e-e2 reroutes the
 * TX path, sio_advance returns immediately at the g_sio_timing_active
 * gate without doing any work. */

#include "psx_cycles.h"
#include "sio.h"

uint64_t psx_cycle_count = 0;

void psx_advance_cycles(uint32_t cycles) {
    if (cycles == 0) return;
    psx_cycle_count += (uint64_t)cycles;
    sio_advance(cycles);
}

uint64_t psx_get_cycle_count(void) {
    return psx_cycle_count;
}

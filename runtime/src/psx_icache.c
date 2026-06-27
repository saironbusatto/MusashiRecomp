/* psx_icache.c — R3000A instruction-cache FETCH cost model (faithful).
 *
 * FAITHFUL_TIMING_PLAN.md axis-2 (I-cache). Transcribed from the in-tree Beetle
 * oracle PS_CPU::ReadInstruction (psxrecomp/beetle-psx/mednafen/psx/cpu.cpp:534-601).
 *
 * The R3000A I-cache is direct-mapped, 4 KB = 1024 words = 256 lines x 4 words
 * (16-byte lines). Beetle models it as ICache[(addr&0xFFC)>>2].TV (the per-word
 * tag+validity); a fetch HITS iff TV == addr (exactly). We mirror only the TV
 * array (we don't need the cached Data — the recompiler/interp already have the
 * instruction; we only charge the FETCH cycle cost and clear the load give-back on
 * a miss, exactly like ReadInstruction).
 *
 * Per fetch:
 *   HIT  (TV == addr): +0, and (crucially) NO load-delay give-back clear.
 *   MISS (TV != addr): clear the pending load give-back (cpu.cpp:542-543), then:
 *     - KSEG1 / cache-disabled (addr >= 0xA0000000): +4, no fill (BIOS ROM case).
 *     - cached (KSEG0/KUSEG): +3, then +1 per word REFILLED from the missing word
 *       to the END of the 4-word line; words before the missing one stay INVALID.
 *       So a miss at word W refills W..3 (cost 3 + (4-W)); a later access to a word
 *       < W in that line misses again. Steady-state hot loops are all hits (+0).
 *
 * Residuals (documented, narrow): the BIU cache-enable bit and the SR cache-isolation
 * bit are not modeled — we treat KSEG0/KUSEG as always cached and KSEG1 as always
 * uncached. This is exact except during the brief pre-cache-enable boot window and
 * inside the cache-flush/isolation routine.
 *
 * State is SHARED by both backends (interp per-instruction; compiled per-cache-line
 * leader) so the cache evolves identically to Beetle for the same fetch stream.
 */
#include "cpu_state.h"
#include <stdint.h>
#include <stdlib.h>

extern void psx_advance_cycles(uint32_t cycles);

/* Opt-in gate (default OFF). The I-cache fetch cost must be charged by BOTH backends
 * or NEITHER — charging it only in the interp (Stage 1) while the compiled path does
 * not would make the two backends disagree on fetch cost in mixed compiled/interp
 * execution (e.g. Tomba 2 overlays), forking timing. So until the compiled emitters
 * also charge it (Stage 2), this is enabled only for measurement (PSX_ICACHE=1, used
 * with PSX_FORCE_INTERP=1 so ALL code is interp'd → consistent). Stage 2 flips the
 * default on once both backends charge it. */
int psx_icache_enabled(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("PSX_ICACHE"); s = (e && e[0] && e[0] != '0'); }
    return s;
}

/* Per-word tag+validity, mirroring Beetle ICache[idx].TV. Init to a value that can
 * never equal an aligned fetch address (aligned addrs have bits 0-1 = 0), so every
 * line starts cold (miss). */
static uint32_t s_icache_tv[1024];

void psx_icache_reset(void) {
    for (int i = 0; i < 1024; i++) s_icache_tv[i] = 0x1u;  /* bit0 set => never matches */
}

/* Charge the fetch cost for the instruction at `addr` and update the cache tags.
 * `addr` is the runtime guest virtual PC (aligned). Gated on PSX_ENABLE_BLOCK_CYCLES. */
void psx_icache_fetch(CPUState* cpu, uint32_t addr) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (!psx_icache_enabled()) return;
    uint32_t idx = (addr & 0xFFCu) >> 2;
    if (s_icache_tv[idx] == addr) return;            /* HIT: +0, no give-back clear */

    /* MISS — clear the pending load give-back (Beetle cpu.cpp:542-543). */
    cpu->read_absorb[cpu->read_absorb_which] = 0u;
    cpu->read_absorb_which = 0u;

    if (addr >= 0xA0000000u) {                       /* KSEG1 / uncached (BIOS ROM) */
        psx_advance_cycles(4u);
        return;
    }

    /* Cached refill (KSEG0/KUSEG). Mark the whole 4-word line invalid, then validate
     * + count the words from the missing one to the line end. */
    uint32_t line = addr & 0xFFFFFFF0u;
    uint32_t bidx = (addr & 0xFF0u) >> 2;            /* first word index of the line */
    s_icache_tv[bidx + 0] = line | 0x0u | 0x2u;
    s_icache_tv[bidx + 1] = line | 0x4u | 0x2u;
    s_icache_tv[bidx + 2] = line | 0x8u | 0x2u;
    s_icache_tv[bidx + 3] = line | 0xCu | 0x2u;
    uint32_t cost = 3u;
    for (uint32_t i = (addr & 0xCu) >> 2; i < 4u; i++) {
        s_icache_tv[bidx + i] &= ~0x2u;              /* valid: TV == that word's addr */
        cost++;
    }
    psx_advance_cycles(cost);
#else
    (void)cpu; (void)addr;
#endif
}

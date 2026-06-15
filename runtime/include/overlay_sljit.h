#ifndef OVERLAY_SLJIT_H
#define OVERLAY_SLJIT_H

/* overlay_sljit — Tier-2 self-contained in-process JIT backend (sljit).
 *
 * Architecture (locked, see SLJIT.md):
 *   Tier 1 (highest) — statically recompiled native (main EXE + discovered
 *                      overlay DLLs). Fastest. Unchanged.
 *   Tier 2 (this)    — the "shard": a fragment recompiled ON THE FLY the first
 *                      time execution would otherwise fall to the interpreter.
 *                      Built in RAM (sljit, sub-ms, no toolchain), registered as
 *                      a native candidate so the NEXT encounter is Tier 2 not
 *                      Tier 3, then persisted so later sessions reload it instead
 *                      of re-discovering through the slow interpreter. This is
 *                      structurally identical to today's gcc capture->compile->
 *                      cache->load path; sljit is just an in-process producer.
 *   Tier 3 (lowest)  — dirty-RAM interpreter (dirty_ram_interp.c). Correctness
 *                      floor for not-yet-discovered code.
 *
 * SAFETY CONTRACT (precision over recall): the emitter compiles only what it can
 * prove it emits identically to the interpreter. The moment it meets any
 * instruction/shape it does not yet support, it ABORTS the whole fragment and
 * returns failure — the caller then runs Tier 3 (interpreter). A partial emitter
 * is therefore always safe: it can never ship a mis-compiled shard, only decline
 * to produce one. gcc remains the default backend; sljit slots in UNDER the
 * proven path and is gated behind backend selection (see overlay_backend.h-style
 * policy below) until it passes the same-state differential.
 */

#include "cpu_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The native shard ABI — identical to overlay_loader's OverlayFn. */
typedef void (*OverlaySljitFn)(CPUState *);

/* Backend selection policy (config + PSX_OVERLAY_BACKEND env override). */
typedef enum {
    OVERLAY_BACKEND_AUTO  = 0, /* gcc if a toolchain is configured, else sljit */
    OVERLAY_BACKEND_GCC   = 1, /* force the spawn-gcc->DLL producer (dev) */
    OVERLAY_BACKEND_SLJIT = 2  /* force the in-process sljit producer       */
} OverlayBackend;

/* Resolve the active backend once at startup. autocompile_configured indicates
 * whether a gcc compile command is wired (game.toml overlay_autocompile_cmd).
 * Reads PSX_OVERLAY_BACKEND (auto|gcc|sljit) env; falls back to `cfg` (from
 * [runtime] overlay_backend) then AUTO. Result is logged and cached. */
OverlayBackend overlay_backend_resolve(const char *cfg, int autocompile_configured);

/* The cached resolution (OVERLAY_BACKEND_AUTO until resolve() runs). */
OverlayBackend overlay_backend_active(void);
const char    *overlay_backend_name(OverlayBackend b);

/* sljit availability — always 1 once the lib is linked (no external dep). */
int overlay_sljit_available(void);

/* Smoke test: JITs a trivial leaf in-process and runs it, returning 1 iff the
 * produced machine code computes the expected value. Proves the sljit codegen +
 * executable-allocator path works in THIS build/host before any real fragment is
 * trusted. Surfaced via the debug server. */
int overlay_sljit_selftest(void);

/* Attempt to JIT the captured fragment at `entry` (guest vram) from `bytes`
 * (the captured image base) / `size`. On success returns a native OverlaySljitFn
 * and the caller registers it as a Tier-2 candidate. On ANY unsupported shape
 * returns NULL (caller falls through to Tier 3, the interpreter) — see the
 * SAFETY CONTRACT above. NOT YET IMPLEMENTED: currently always returns NULL
 * (every fragment safely declines to the interpreter). */
OverlaySljitFn overlay_sljit_try_compile(uint32_t entry,
                                         const uint8_t *bytes, uint32_t size,
                                         uint32_t image_base_vram);

/* Diagnostics counters for the debug server. */
void overlay_sljit_get_status(int *available, int *selftest_ok,
                              uint64_t *compiles, uint64_t *declines,
                              uint64_t *bytes_emitted);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_SLJIT_H */

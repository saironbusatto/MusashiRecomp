/* code_provider.h — the backend-agnostic code-production seam (SLJIT.md §3).
 *
 * One interface the overlay capture/dispatch spine talks to instead of calling
 * a specific backend directly. The validated gcc spawn->DLL path becomes its
 * first implementation; the in-process sljit JIT (SLJIT.md §7 step 4) becomes
 * the second. Everything that makes overlays correct — content-keyed multi-
 * candidate dispatch, per-call live-byte validation, the self-mod blacklist,
 * the coverage manifest — is unchanged and backend-blind (SLJIT.md §2).
 *
 * Two production lifecycles, BOTH expressed here because the two backends model
 * production differently and a provider implements whichever it supports:
 *
 *   - BATCH / ASYNC  (request / busy / poll_main): the gcc model. Kicked by the
 *     autocapture tick, spawns a compiler off-thread, applied later via a cache
 *     rescan on the emu thread. Seconds of latency; many fragments per run.
 *   - SYNC / ON-MISS (compile_fragment): the sljit model. JIT a single fragment
 *     in-process at a dispatch miss, sub-ms, return a native fn the caller
 *     registers immediately. (Wired into the dispatch path in SLJIT.md step 5;
 *     defined here so the abstraction is honest about both modes, not gcc-only.)
 *
 * A provider sets the hooks it supports and leaves the rest NULL; callers
 * null-check before invoking. The gcc provider's batch hooks are thin pass-
 * throughs to autocompile_* — behavior is byte-for-byte what it was before the
 * abstraction (this is a pure refactor; selection defaults to gcc).
 */
#ifndef PSXRECOMP_CODE_PROVIDER_H
#define PSXRECOMP_CODE_PROVIDER_H

#include "cpu_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native shard ABI — identical to overlay_loader's OverlayFn / OverlaySljitFn. */
typedef void (*CodeProviderFn)(CPUState *);

typedef struct CodeProvider {
    const char *name;                 /* "gcc" | "sljit"                       */
    int  (*available)(void);          /* gcc: a compile cmd is configured;
                                         sljit: always (no external toolchain) */

    /* Batch/async production (autocapture-driven). request() returns 1 if a
     * compile was started. busy() is 1 while one is in flight. poll_main() runs
     * on the emu thread to apply a finished compile (cache rescan). Any may be
     * NULL on a provider that does not do batch production. */
    int  (*request)(void);
    int  (*busy)(void);
    void (*poll_main)(void);

    /* Synchronous on-miss production of one fragment. Returns a native fn, or
     * NULL to decline (caller falls through to the interpreter — the always-safe
     * precision-over-recall floor). gcc leaves this NULL (a compiler spawn is
     * far too slow for the dispatch path); sljit JITs in-process. A NULL hook is
     * treated by callers as "always declines". */
    CodeProviderFn (*compile_fragment)(uint32_t entry, const uint8_t *bytes,
                                       uint32_t size, uint32_t image_base_vram);
} CodeProvider;

/* Resolve the active backend (overlay_backend_resolve) and cache the matching
 * provider. cfg_backend = [runtime] overlay_backend ("auto"|"gcc"|"sljit", may
 * be NULL/empty for auto); gcc_configured = autocompile_configured(). Call once
 * at overlay-cache init, on the emu thread, before the run loop starts. */
void code_provider_init(const char *cfg_backend, int gcc_configured);

/* The active provider. Never NULL — defaults to the gcc provider before
 * code_provider_init() runs, so any early caller is safe. */
const CodeProvider *code_provider_active(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_CODE_PROVIDER_H */

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
 *     registers immediately. Wired into the dispatch path (overlay_loader.c
 *     try_sljit_region); the sljit shard is then VALIDATED via the same-state
 *     diff before it runs live (see SLJIT.md §11 / overlay_loader_dispatch).
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

/* Result of a synchronous on-miss compile. fn == NULL means the provider
 * declined (caller runs the interpreter). On success [code_lo, code_lo+code_len)
 * is the PHYS byte-range the fragment was compiled from — the caller registers a
 * candidate over it for per-call live-byte validation (the backend-neutral
 * equivalent of the gcc .ranges manifest). */
typedef struct {
    CodeProviderFn fn;        /* NULL = declined */
    uint32_t       code_lo;   /* phys start of compiled code range */
    uint32_t       code_len;  /* byte length of compiled code range */
} CompiledFragment;

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

    /* Synchronous on-miss production of one fragment. Fills *out (out->fn NULL =
     * declined → caller runs the interpreter, the always-safe precision-over-
     * recall floor). gcc leaves this hook NULL (a compiler spawn is far too slow
     * for the dispatch path); sljit JITs in-process. A NULL hook == always
     * declines. `bytes`/`size`/`image_base_vram` describe the image to decode
     * from (typically live RAM: bytes = RAM base, image_base_vram = 0). */
    void (*compile_fragment)(uint32_t entry, const uint8_t *bytes, uint32_t size,
                             uint32_t image_base_vram, CompiledFragment *out);
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

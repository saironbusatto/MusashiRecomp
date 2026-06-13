#ifndef OVERLAY_API_H
#define OVERLAY_API_H

/* Shared ABI between psx-runtime.exe and overlay DLLs.
 *
 * After LoadLibrary, the runtime calls overlay_init() with this struct.
 * The DLL stores the pointers and routes all cross-module calls through them.
 */

#include "cpu_state.h"
#include <stdint.h>

/* ABI version exported by every overlay DLL as `overlay_abi()`.  The loader
 * rejects (and deletes, so autocompile regenerates) any DLL whose version
 * doesn't match — including all pre-versioning DLLs, which lack the export.
 *
 * v2: dispatch call-contract (Bug D family) — DLL call sites carry (ra, sp)
 *     contract checks and share the runtime's bail state via the appended
 *     callback pointers below.
 * v3: widescreen backdrop screenX squash ([widescreen.backdrop] x_sites) —
 *     overlay backdrop handlers now emit psx_ws_backdrop_x() at their screenX
 *     store. This is an EMIT-content change (not a struct-ABI change), but the
 *     version is the cache discriminator: bumping it forces the loader to
 *     reject pre-backdrop DLLs so autocompile regenerates them with the squash.
 *     (The deferred follow-up is to auto-derive this tag from a hash of the
 *     widescreen config + emit headers; until then it's a manual bump.) */
#define PSX_OVERLAY_ABI_VERSION 3

/* Codegen flavor of the recompiled output the overlays + runtime were built
 * against. Overlays are keyed in the cache by guest-bytes CRC, which is
 * flavor-BLIND — the same guest overlay yields the same filename whether it was
 * compiled by the base recompiler or a variant (e.g. widescreen, which emits
 * extra runtime symbols + GTE adjustments). Without a flavor tag a widescreen
 * DLL and a base DLL for the same overlay collide. The flavor is folded into
 * overlay_abi() (high 16 bits) so the loader's existing ABI gate rejects (and
 * regenerates) any DLL whose flavor differs from the running build's, keeping
 * caches separate even in a shared directory.
 *
 * Base/master MUST stay 0 so the tag == PSX_OVERLAY_ABI_VERSION (backward
 * compatible with all existing base-flavor DLLs). A variant build overrides it
 * (e.g. -DPSX_OVERLAY_FLAVOR=1 for widescreen) for BOTH the runtime and
 * compile_overlays.py (its --flavor arg). */
#ifndef PSX_OVERLAY_FLAVOR
#define PSX_OVERLAY_FLAVOR 0
#endif

/* Combined tag exported by overlay_abi() and checked by the loader. */
#define PSX_OVERLAY_ABI_TAG \
    ((int)((PSX_OVERLAY_ABI_VERSION & 0xFFFF) | ((PSX_OVERLAY_FLAVOR & 0xFFFF) << 16)))

typedef struct {
    /* Core dispatch: routes call_by_address() and out-of-overlay jal */
    void (*dispatch_call)(CPUState *cpu, uint32_t addr, uint32_t ra);
    /* Interrupt check: called after every function return in overlay */
    void (*check_interrupts)(CPUState *cpu);
    /* GTE coprocessor 2 execution */
    void (*gte_execute)(CPUState *cpu, uint32_t cmd);
    /* MIPS syscall (break/syscall instructions) */
    void (*psx_syscall)(CPUState *cpu, uint32_t code);
    /* Unresolved dispatch target */
    void (*psx_unknown_dispatch)(CPUState *cpu, uint32_t addr, uint32_t phys);
    /* Debug instrumentation: called at every function entry (may be NULL) */
    void (*log_call_entry)(uint32_t func_addr);
    /* RestoreState/ReturnFromException longjmp escape (interrupts.c). The
     * recompiler emits this at longjmp-return sites in exception-context
     * kernel code (the install-slot / kernel-window class). Appended LAST:
     * overlay_init copies the struct by value, so older DLLs built against
     * the shorter struct simply never read this member. */
    void (*psx_restore_state_escape)(void);
    /* Call-contract state shared with the runtime (ABI v2; see the contract
     * model in cpu_state.h).  DLL code reads the bail flag and bumps the
     * counters through these pointers. */
    int      *call_bail_flag;
    uint64_t *bail_first;
    uint64_t *bail_resolved;
    /* Widescreen hooks (ABI v3). The recompiler can emit psx_ws_* calls into
     * OVERLAY code (backdrop screenX squash; and, for other games, sprite-tag
     * or cull sites that resolve into overlays). These compute against the
     * host's live widescreen state (gpu.c), so the DLL forwards to the runtime
     * through these. Appended LAST (struct grows back-compatibly); may be NULL
     * on a host that predates them — the DLL glue falls back to identity. */
    int  (*ws_backdrop_x)(int x);
    int  (*ws_x_margin)(void);
    void (*ws_sprite_tag)(CPUState *cpu);
} OverlayCallbacks;

#ifdef __cplusplus
extern "C" {
#endif

/* Exported by every overlay DLL.  Call once after LoadLibrary. */
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void overlay_init(const OverlayCallbacks *cbs);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_API_H */

/* code_provider.c — the two CodeProvider implementations + active selection.
 * See code_provider.h for the abstraction and SLJIT.md §3/§6/§7 for the plan.
 *
 * This is the step-2 seam: the gcc provider is a thin pass-through over the
 * validated autocompile_* path (no behavior change), and the sljit provider
 * wraps the in-process JIT. Which one is active is decided once at init by
 * overlay_backend_resolve (env PSX_OVERLAY_BACKEND > [runtime] overlay_backend >
 * auto; auto => gcc if a compile cmd is wired, else sljit). gcc is the default. */

#include "code_provider.h"
#include "autocompile.h"
#include "overlay_sljit.h"

/* ---- gcc provider: pass-through to the validated spawn->DLL->rescan path --- */
/* The hook signatures match autocompile_* exactly, so we point straight at
 * them — no wrappers, no behavior change. compile_fragment stays NULL: a
 * compiler spawn is orders of magnitude too slow for the dispatch path, so gcc
 * never produces a fragment synchronously (it self-improves via the batch path
 * + autocapture instead). */
static const CodeProvider s_gcc = {
    /* name             */ "gcc",
    /* available        */ autocompile_configured,
    /* request          */ autocompile_request,
    /* busy             */ autocompile_busy,
    /* poll_main        */ autocompile_poll_main,
    /* compile_fragment */ NULL,
};

/* ---- sljit provider: in-process JIT (sync on-miss only, v1) ---------------- */
/* No batch/async path: sljit produces synchronously at a dispatch miss, so
 * request/busy/poll_main are inert. compile_fragment delegates to the MIPS->sljit
 * emitter (overlay_sljit_try_compile), which compiles the supported instruction
 * classes and declines the rest (fn=NULL) — safe by construction (precision over
 * recall). The returned shard is validated via the same-state diff before live. */
static int  sljit_request(void)   { return 0; }
static int  sljit_busy(void)      { return 0; }
static void sljit_poll_main(void) { }

static void sljit_compile_fragment(uint32_t entry, const uint8_t *bytes,
                                   uint32_t size, uint32_t image_base_vram,
                                   CompiledFragment *out) {
    OverlaySljitResult r = {0};
    overlay_sljit_try_compile(entry, bytes, size, image_base_vram, &r);
    /* OverlaySljitFn and CodeProviderFn are the same void fn(CPUState*) type. */
    out->fn       = (CodeProviderFn)r.fn;
    out->code_lo  = r.code_lo;
    out->code_len = r.code_len;
}

static const CodeProvider s_sljit = {
    /* name             */ "sljit",
    /* available        */ overlay_sljit_available,
    /* request          */ sljit_request,
    /* busy             */ sljit_busy,
    /* poll_main        */ sljit_poll_main,
    /* compile_fragment */ sljit_compile_fragment,
};

/* ---- active selection ------------------------------------------------------ */
/* Default to gcc so any caller before code_provider_init() (there should be
 * none — init runs at overlay-cache setup) gets the proven path. */
static const CodeProvider *s_active = &s_gcc;

void code_provider_init(const char *cfg_backend, int gcc_configured) {
    /* overlay_backend_resolve resolves AUTO to GCC or SLJIT and never returns
     * AUTO, so this maps cleanly to one of the two providers. It also logs the
     * decision (surfaced via the sljit_status debug command). */
    OverlayBackend b = overlay_backend_resolve(cfg_backend, gcc_configured);
    s_active = (b == OVERLAY_BACKEND_SLJIT) ? &s_sljit : &s_gcc;
}

const CodeProvider *code_provider_active(void) { return s_active; }

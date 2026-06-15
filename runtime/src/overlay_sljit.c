/* overlay_sljit.c — Tier-2 self-contained in-process JIT backend (sljit).
 * See overlay_sljit.h for the tier model and the precision-over-recall SAFETY
 * CONTRACT. This file currently provides: backend-selection policy, sljit
 * availability + a real codegen smoke test, and the try_compile entry point
 * that (until the MIPS->sljit emitter lands) safely declines every fragment to
 * the interpreter. The validated gcc path is untouched. */

#include "overlay_sljit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "sljitLir.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* ---- counters ---------------------------------------------------------- */
static OverlayBackend s_active   = OVERLAY_BACKEND_AUTO;
static int            s_resolved = 0;
static int            s_selftest_ok = -1; /* -1 = not run */
static uint64_t       s_compiles = 0;
static uint64_t       s_declines = 0;
static uint64_t       s_bytes    = 0;
static char           s_last_msg[256];

static void sljit_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_last_msg, sizeof(s_last_msg), fmt, ap);
    va_end(ap);
}

/* ---- backend selection policy ----------------------------------------- */
const char *overlay_backend_name(OverlayBackend b) {
    switch (b) {
        case OVERLAY_BACKEND_GCC:   return "gcc";
        case OVERLAY_BACKEND_SLJIT: return "sljit";
        default:                    return "auto";
    }
}

static OverlayBackend parse_backend(const char *s, OverlayBackend dflt) {
    if (!s || !*s) return dflt;
    if (!strcmp(s, "gcc"))   return OVERLAY_BACKEND_GCC;
    if (!strcmp(s, "sljit")) return OVERLAY_BACKEND_SLJIT;
    if (!strcmp(s, "auto"))  return OVERLAY_BACKEND_AUTO;
    return dflt;
}

OverlayBackend overlay_backend_resolve(const char *cfg, int autocompile_configured) {
    /* Precedence: env PSX_OVERLAY_BACKEND > game.toml [runtime] overlay_backend
     * (cfg) > AUTO. AUTO prefers gcc when a compile command is wired (a dev
     * machine), else sljit (self-contained production / toolchain-less dev). */
    OverlayBackend want = parse_backend(getenv("PSX_OVERLAY_BACKEND"),
                                        parse_backend(cfg, OVERLAY_BACKEND_AUTO));
    OverlayBackend eff = want;
    if (want == OVERLAY_BACKEND_AUTO)
        eff = autocompile_configured ? OVERLAY_BACKEND_GCC : OVERLAY_BACKEND_SLJIT;

    s_active   = eff;
    s_resolved = 1;
    sljit_log("backend resolved: want=%s effective=%s (autocompile=%d)",
              overlay_backend_name(want), overlay_backend_name(eff),
              autocompile_configured);
    return eff;
}

OverlayBackend overlay_backend_active(void) { return s_active; }

int overlay_sljit_available(void) { return 1; }

/* ---- smoke test: JIT a trivial leaf and run it ------------------------- */
/* Produces machine code for `sljit_sw f(sljit_sw a) { return a + 1234; }`,
 * runs it, and checks the result. Proves the codegen + executable allocator
 * work in this build/host. */
typedef sljit_sw (SLJIT_FUNC *SmokeFn)(sljit_sw);

int overlay_sljit_selftest(void) {
    if (s_selftest_ok >= 0) return s_selftest_ok;

    struct sljit_compiler *C = sljit_create_compiler(NULL);
    if (!C) { s_selftest_ok = 0; sljit_log("selftest: create_compiler failed"); return 0; }

    /* one arg (W) -> arrives in saved reg S0; one scratch, one saved, no locals */
    sljit_emit_enter(C, 0, SLJIT_ARGS1(W, W), 1, 1, 0);
    sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_S0, 0, SLJIT_IMM, 1234);
    sljit_emit_return(C, SLJIT_MOV, SLJIT_R0, 0);

    void *code = sljit_generate_code(C, 0, NULL);
    sljit_uw code_size = sljit_get_generated_code_size(C);
    sljit_free_compiler(C);

    if (!code) { s_selftest_ok = 0; sljit_log("selftest: generate_code failed"); return 0; }

    SmokeFn fn = (SmokeFn)code;
    sljit_sw got = fn(1000);
    sljit_free_code(code, NULL);

    s_selftest_ok = (got == 2234) ? 1 : 0;
    sljit_log("selftest: f(1000)=%ld expected=2234 ok=%d code_size=%lu",
              (long)got, s_selftest_ok, (unsigned long)code_size);
    return s_selftest_ok;
}

/* ---- fragment compile (NOT YET IMPLEMENTED) ---------------------------- */
OverlaySljitFn overlay_sljit_try_compile(uint32_t entry,
                                         const uint8_t *bytes, uint32_t size,
                                         uint32_t image_base_vram) {
    (void)entry; (void)bytes; (void)size; (void)image_base_vram;
    /* SAFETY CONTRACT: the MIPS->sljit emitter is not built yet, so every
     * fragment safely declines to the interpreter (Tier 3). When the emitter
     * lands it will compile what it can prove and continue to return NULL on
     * any unsupported instruction/shape. */
    s_declines++;
    return NULL;
}

void overlay_sljit_get_status(int *available, int *selftest_ok,
                              uint64_t *compiles, uint64_t *declines,
                              uint64_t *bytes_emitted) {
    if (available)     *available     = overlay_sljit_available();
    if (selftest_ok)   *selftest_ok   = s_selftest_ok;
    if (compiles)      *compiles      = s_compiles;
    if (declines)      *declines      = s_declines;
    if (bytes_emitted) *bytes_emitted = s_bytes;
}

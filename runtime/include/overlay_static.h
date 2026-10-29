#ifndef OVERLAY_STATIC_H
#define OVERLAY_STATIC_H

/* Statically-linked overlays (B-2): overlay code recompiled ahead of time and
 * linked into the executable instead of loaded from DLLs.
 *
 * tools/compile_overlays.py --static emits one C file per overlay image, each
 * ending in a PsxStaticOverlay table, plus overlays_static_index.c listing all
 * of them. overlay_loader_init() registers every entry as a loader candidate,
 * so a static function runs only while live RAM matches the bytes it was
 * compiled from — the same per-function crc gate as cached DLLs. Many overlays
 * share one load address; that gate is what picks the right one.
 *
 * The index is linked only when the game target provides it
 * (PSX_HAS_STATIC_OVERLAYS); without it the loader registers nothing. */

#include "cpu_state.h"
#include <stdint.h>

typedef struct {
    uint32_t entry;        /* phys entry address                               */
    uint32_t crc;          /* crc32 of the code ranges the function was built from */
    uint32_t probe;        /* code word at entry: cheap reject before the crc  */
    uint32_t range_first;  /* index of its first (lo, len) pair in ranges[]     */
    uint32_t nranges;
    void   (*fn)(CPUState *);
} PsxStaticOvFn;

typedef struct {
    uint32_t             nfns;
    const PsxStaticOvFn *fns;
    const uint32_t      *ranges;   /* (phys lo, byte length) pairs             */
} PsxStaticOverlay;

extern const PsxStaticOverlay *const psx_static_overlays[];
extern const uint32_t psx_static_overlay_count;

#endif

#include "overlay_loader.h"
#include "overlay_api.h"
#include "code_provider.h"
#include "overlay_sljit.h"
#include "crc32.h"
#include "dirty_ram_interp.h"
#include "interrupts.h"
#include "debug_server.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

/* ============================================================================
 * Inc3 — Per-entry validity + multi-candidate dispatch (design doc §8)
 *
 * Validity is tracked per *compiled entry*, not per dirty region. Each entry
 * carries its tight code byte-ranges (from the recompiler's per-function walk,
 * emitted as a {base}_{crc}.ranges manifest beside the DLL), a content hash of
 * those ranges, and a page-generation snapshot. A write to a watched page bumps
 * that page's generation (memory.c); dispatch lazily re-hashes an entry only
 * when the generation over its ranges changed. This:
 *   - kills the false-invalidation churn of the old [fn_lo,fn_hi) region flag
 *     (data interleaved between functions no longer nukes the whole region), and
 *   - makes reload-on-return gradual and automatic — each entry flips back to
 *     native the moment its own code bytes reappear (hash matches), with no
 *     region-wide atomic threshold.
 *
 * PC -> candidate list: different overlays can reuse the same RAM address
 * (Tomba loads village and overworld both at 0x800E7xxx). A single-entry table
 * would let the later DLL clobber the earlier candidate, making reload-on-return
 * impossible. So each PC maps to a chain of candidates; dispatch picks the one
 * whose code hash matches live RAM.
 *
 * §8.3 RESOLVED: jump tables compile to `switch (live register)` with a
 * call_by_address default, so table contents are an optimization, not a
 * correctness dependency. The dependency set is the entry's code bytes only.
 * ==========================================================================*/

typedef void (*OverlayFn)(CPUState *);

#define MAX_CODE_RANGES 16   /* code ranges per function (coalesced; usually 1) */

enum { ENTRY_VALID = 0, ENTRY_INVALID = 1, ENTRY_BLACKLIST = 2 };

typedef struct {
    uint32_t  addr;                      /* phys entry address                 */
    OverlayFn fn;
    uint32_t  range_lo[MAX_CODE_RANGES]; /* phys code-range starts             */
    uint32_t  range_len[MAX_CODE_RANGES];
    int       nranges;
    uint32_t  crc_code;                  /* hash of code ranges at registration*/
    uint32_t  val_gen;                   /* pagegen sum when last (in)validated*/
    int       state;                     /* ENTRY_VALID/INVALID/BLACKLIST      */
    int       dll;                       /* source DLL index                   */
    int       next;                      /* next candidate at same addr, -1 end*/
} Candidate;

#define CAND_CAP   16384
static Candidate s_cand[CAND_CAP];
static int       s_cand_n = 0;

/* Open-addressed index: phys entry addr -> head candidate index (-1 sentinel
 * stored as chain terminator on each Candidate). addr 0 = empty slot. */
#define IDX_CAP  32768u
#define IDX_MASK (IDX_CAP - 1u)
typedef struct { uint32_t addr; int head; } IdxSlot;
static IdxSlot s_idx[IDX_CAP];

static int idx_head(uint32_t phys) {
    uint32_t h = (phys * 2654435761u) & IDX_MASK;
    for (uint32_t i = 0; i < IDX_CAP; i++) {
        uint32_t k = (h + i) & IDX_MASK;
        if (s_idx[k].addr == 0)    return -1;
        if (s_idx[k].addr == phys) return s_idx[k].head;
    }
    return -1;
}
static void idx_set_head(uint32_t phys, int head) {
    uint32_t h = (phys * 2654435761u) & IDX_MASK;
    for (uint32_t i = 0; i < IDX_CAP; i++) {
        uint32_t k = (h + i) & IDX_MASK;
        if (s_idx[k].addr == 0 || s_idx[k].addr == phys) {
            s_idx[k].addr = phys;
            s_idx[k].head = head;
            return;
        }
    }
}

/* Active native-entry stack (self-modification detection, §8.5). */
static int s_active_stack[64];
static int s_active_depth = 0;

/* ---- Observability (recomp-debug: measure, don't eyeball) -------------- */
/* Always-on ring of native overlay calls, so the FIRST divergence / last
 * function executed before a corruption can be read back from the window of
 * interest — never "arm a trace then hope". s_native_inprogress holds the entry
 * currently executing (nonzero at dump => a native fn was entered and never
 * returned: a freeze INSIDE native code, the strongest single suspect). */
#define NRING_CAP 1024
typedef struct { uint32_t addr; uint32_t crc; uint64_t seq; int returned; } NRingEnt;
static NRingEnt s_nring[NRING_CAP];
static uint32_t s_nring_pos = 0;
static uint64_t s_nring_seq = 0;
static uint32_t s_native_inprogress = 0;   /* addr of fn currently in native, 0 = none */
static uint64_t s_native_calls_total = 0;

/* Runtime A/B: when 0, candidates are still hashed/validated and recorded, but
 * NEVER executed native — execution falls to the interpreter. Lets us prove
 * whether native EXECUTION is the cause without a rebuild or losing candidate
 * visibility. Default on. */
static int s_native_exec = 1;
static uint64_t s_would_run_native = 0;   /* matched but skipped (exec off)   */

void overlay_loader_set_native_exec(int on) { s_native_exec = on ? 1 : 0; }
int  overlay_loader_get_native_exec(void)   { return s_native_exec; }
/* Address of the native overlay function currently executing (0 if none).
 * Used by the event-timeline ring to tag an event's execution mode. */
uint32_t overlay_loader_get_inprogress(void) { return s_native_inprogress; }

/* Same-state differential — defined fully below; declared here for dispatch. */
static int  s_diff_mode = 0;
static int  s_in_shadow = 0;
static void run_shadow_diff(CPUState *cpu, Candidate *c, uint32_t addr);

/* ---- Counters (surfaced via overlay_loader_status) --------------------- */
static int      s_ndlls          = 0;   /* DLLs LoadLibrary'd                 */
static int      s_valid_count    = 0;   /* candidates currently VALID         */
static uint64_t s_disp_native    = 0;
static uint64_t s_disp_interp    = 0;
static uint64_t s_stale_blocked  = 0;   /* dispatches skipped (candidate !valid)*/
static uint32_t s_invalidations  = 0;   /* VALID -> INVALID transitions       */
static uint32_t s_revalidations  = 0;   /* INVALID -> VALID (reload-on-return) */
static uint32_t s_rehashes       = 0;   /* code-range hashes computed         */
static uint32_t s_rehash_miss    = 0;   /* hashes that didn't match crc_code  */
static uint32_t s_last_crc       = 0;
static uint32_t s_no_manifest    = 0;   /* exports skipped (no manifest range)*/
static uint32_t s_selfmod        = 0;   /* entries blacklisted (self-mod)     */
static uint32_t s_sljit_registered = 0; /* sljit Tier-2 shards registered     */
static uint32_t s_last_write_pc   = 0;
static uint32_t s_last_write_addr = 0;
static uint32_t s_last_write_size = 0;

extern uint8_t *memory_get_ram_ptr(void);
extern uint32_t overlay_watch_pagegen_sum(uint32_t phys, uint32_t len);

/* ---- Per-candidate hash / generation over its code ranges -------------- */

static uint32_t cand_crc(const Candidate *c) {
    const uint8_t *ram = memory_get_ram_ptr();
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < c->nranges; i++)
        crc = crc32_update(crc, ram + c->range_lo[i], c->range_len[i]);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t cand_gensum(const Candidate *c) {
    uint32_t s = 0;
    for (int i = 0; i < c->nranges; i++)
        s += overlay_watch_pagegen_sum(c->range_lo[i], c->range_len[i]);
    return s;
}

/* ---- Per-DLL code-range manifest --------------------------------------- */
/* Minimal line format emitted by tools/compile_overlays.py beside each DLL:
 *   F <entry_hex>            start a function (entry = virtual export addr)
 *   R <lo_hex> <len_hex>     a code byte-range (virtual addr, byte length)
 * The recompiler's per-function instruction walk yields exactly the executed
 * code bytes — interleaved jump tables / constant pools are excluded, which is
 * what makes the hash stable across reloads. */
typedef struct {
    uint32_t entry;
    uint32_t crc;       /* expected hash of the compiled-from code bytes       */
    int      has_crc;   /* 1 if the manifest supplied the authoritative hash   */
    uint32_t lo[MAX_CODE_RANGES];
    uint32_t len[MAX_CODE_RANGES];
    int      n;
} ManFn;

static ManFn *parse_manifest(const char *path, int *out_n) {
    *out_n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    int cap = 1024, n = 0;
    ManFn *arr = (ManFn *)malloc(sizeof(ManFn) * cap);
    if (!arr) { fclose(f); return NULL; }
    char line[160];
    ManFn *cur = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'F') {
            uint32_t e = 0, crc = 0;
            int got = sscanf(line + 1, "%x %x", &e, &crc);
            if (got >= 1) {
                if (n >= cap) {
                    cap *= 2;
                    ManFn *na = (ManFn *)realloc(arr, sizeof(ManFn) * cap);
                    if (!na) break;
                    arr = na;
                }
                cur = &arr[n++];
                cur->entry   = e;
                cur->crc     = crc;
                cur->has_crc = (got >= 2) ? 1 : 0;
                cur->n = 0;
            }
        } else if (line[0] == 'R' && cur) {
            uint32_t lo, len;
            if (sscanf(line + 1, "%x %x", &lo, &len) == 2 &&
                cur->n < MAX_CODE_RANGES && len > 0) {
                cur->lo[cur->n]  = lo;
                cur->len[cur->n] = len;
                cur->n++;
            }
        }
    }
    fclose(f);
    *out_n = n;
    return arr;
}

static ManFn *man_find(ManFn *arr, int n, uint32_t entry) {
    for (int i = 0; i < n; i++)
        if (arr[i].entry == entry) return &arr[i];
    return NULL;
}

/* ---- Candidate registration -------------------------------------------- */

static void cand_register(uint32_t phys, OverlayFn fn, const ManFn *m, int dll) {
    if (s_cand_n >= CAND_CAP) return;
    int idx = s_cand_n++;
    Candidate *c = &s_cand[idx];
    c->addr    = phys;
    c->fn      = fn;
    c->dll     = dll;
    c->state   = ENTRY_VALID;
    c->nranges = 0;
    for (int i = 0; i < m->n && c->nranges < MAX_CODE_RANGES; i++) {
        c->range_lo[c->nranges]  = m->lo[i] & 0x1FFFFFFFu;
        c->range_len[c->nranges] = m->len[i];
        c->nranges++;
    }
    /* Watch the code pages so future writes bump their generation. */
    extern void overlay_watch_set_range(uint32_t phys, uint32_t len);
    for (int i = 0; i < c->nranges; i++)
        overlay_watch_set_range(c->range_lo[i], c->range_len[i]);

    /* crc_code is the AUTHORITATIVE hash of the bytes the recompiler compiled
     * from (supplied by the manifest) — NOT a sample of live RAM at this instant
     * (registration is a single transient moment; the overlay may not be fully
     * present yet, and other overlays sharing the merged DLL are not present at
     * all). A candidate is callable iff live RAM matches this compiled-from hash,
     * which makes validity timing-independent and reload-on-return work. */
    if (m->has_crc) {
        c->crc_code = m->crc;
    } else {
        c->crc_code = cand_crc(c);   /* legacy manifest without hashes */
    }
    c->val_gen = cand_gensum(c);
    c->state   = (cand_crc(c) == c->crc_code) ? ENTRY_VALID : ENTRY_INVALID;
    c->next    = idx_head(phys);
    idx_set_head(phys, idx);
    if (c->state == ENTRY_VALID) s_valid_count++;
}

/* ---- sljit Tier-2 shard registration (SLJIT.md §7 step 5) -------------- */
static void loader_log(const char *fmt, ...);   /* defined below */
/* Register an in-process JIT shard as a native candidate, parallel to
 * cand_register but without a .ranges manifest: the shard was JIT'd from the
 * live RAM bytes over [lo, lo+len), so crc_code is hashed from those same bytes
 * — a later dispatch runs the shard iff the code is still byte-identical
 * (reload-on-return / self-mod safety, the same live-byte contract gcc
 * candidates obey). */
static void register_sljit_candidate(uint32_t phys, OverlayFn fn,
                                     uint32_t lo, uint32_t len) {
    if (s_cand_n >= CAND_CAP || len == 0) return;
    int idx = s_cand_n++;
    Candidate *c = &s_cand[idx];
    c->addr        = phys & 0x1FFFFFFFu;
    c->fn          = fn;
    c->dll         = -1;            /* sentinel: sljit shard, not a DLL */
    c->nranges     = 1;
    c->range_lo[0] = lo & 0x1FFFFFFFu;
    c->range_len[0] = len;
    extern void overlay_watch_set_range(uint32_t phys, uint32_t len);
    overlay_watch_set_range(c->range_lo[0], c->range_len[0]);
    c->crc_code = cand_crc(c);      /* live bytes == the bytes we JIT'd from */
    c->val_gen  = cand_gensum(c);
    c->state    = ENTRY_VALID;
    c->next     = idx_head(c->addr);
    idx_set_head(c->addr, idx);
    s_valid_count++;
    s_sljit_registered++;
    loader_log("sljit shard registered 0x%08X [%u bytes]", c->addr, len);
}

/* JIT-on-miss memo: phys entries already attempted (compiled or declined), so a
 * declined fragment isn't re-attempted every dispatch. */
#define MAX_SLJIT_TRIED 512
static uint32_t s_sljit_tried[MAX_SLJIT_TRIED];
static int      s_sljit_tried_n = 0;
static int sljit_already_tried(uint32_t phys) {
    for (int i = 0; i < s_sljit_tried_n; i++)
        if (s_sljit_tried[i] == phys) return 1;
    return 0;
}
static void sljit_mark_tried(uint32_t phys) {
    if (s_sljit_tried_n < MAX_SLJIT_TRIED) s_sljit_tried[s_sljit_tried_n++] = phys;
}

/* Attempt an in-process JIT of the leaf function at `phys` from live RAM, and
 * register the shard as a candidate on success. Gated by the caller to the
 * validation configuration only (backend==sljit + diff_mode); see the dispatch
 * hook. Decodes from live RAM exactly as the interpreter does. */
static void try_sljit_region(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    const CodeProvider *cp = code_provider_active();
    if (!cp->compile_fragment) return;
    if (sljit_already_tried(phys)) return;
    extern int dirty_ram_is_dirty(uint32_t phys);
    if (!dirty_ram_is_dirty(phys)) return;   /* only JIT real runtime code */
    sljit_mark_tried(phys);
    uint8_t *ram = memory_get_ram_ptr();
    if (!ram) return;
    CompiledFragment frag = {0};
    /* Decode from live RAM: bytes = RAM base, image_base_vram = 0 ⇒ byte offset =
     * (entry & 0x1FFFFFFF) = phys. Pass the VIRTUAL entry so return_pc and
     * jal/J targets carry the KSEG bits the guest uses — the interpreter computes
     * pc from the virtual address, and saved $ra values must match byte-exact. */
    cp->compile_fragment(addr, ram, 2u * 1024u * 1024u, 0u, &frag);
    if (frag.fn)
        register_sljit_candidate(phys, (OverlayFn)frag.fn, frag.code_lo, frag.code_len);
}

/* ---- Global state ------------------------------------------------------ */

static char s_cache_dir[512];
static char s_game_id[64];
static int  s_active = 0;

/* Rule 3: no stderr logging. Most recent loader event recorded here and
 * surfaced through overlay_loader_status. */
static char s_last_msg[256] = {0};

static void loader_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_msg, sizeof(s_last_msg), fmt, ap);
    va_end(ap);
}

const char *overlay_loader_last_msg(void) { return s_last_msg; }

/* ---- Cache index: region_start -> dll path ----------------------------- */

#define CACHE_IDX_CAP 256
typedef struct { uint32_t region_start; char path[768]; } CacheEntry;
static CacheEntry s_cache_idx[CACHE_IDX_CAP];
static int        s_cache_idx_count = 0;

static int cache_idx_has_path(const char *path) {
    for (int i = 0; i < s_cache_idx_count; i++)
        if (strcmp(s_cache_idx[i].path, path) == 0) return 1;
    return 0;
}

/* Canonical cache arch-abi tag (see SLJIT.md §4 — caches are namespaced per
 * backend AND per target so a Windows-x64 gcc DLL and, later, a same-OS arm64
 * or an sljit blob for the same fragment never comingle). compile_overlays.py
 * computes the IDENTICAL string from platform.system()/machine(); keep the two
 * mappings in lockstep ("<os>-<arch>": win|linux|macos + x64|arm64|x86). */
#if defined(_WIN32)
#  define PSX_OL_OS "win"
#elif defined(__APPLE__)
#  define PSX_OL_OS "macos"
#else
#  define PSX_OL_OS "linux"
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#  define PSX_OL_ARCH "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#  define PSX_OL_ARCH "x64"
#elif defined(__i386__) || defined(_M_IX86)
#  define PSX_OL_ARCH "x86"
#else
#  define PSX_OL_ARCH "unknown"
#endif
#define PSX_OVERLAY_ARCH_ABI PSX_OL_OS "-" PSX_OL_ARCH

const char *overlay_loader_arch_abi(void) { return PSX_OVERLAY_ARCH_ABI; }

/* Scan one directory for <addr8>_<crc8>.dll cache entries into the index.
 * `dir` is a full directory path. Idempotent (skips already-indexed paths). */
static void scan_one_cache_dir(const char *dir) {
#ifdef _WIN32
    char pattern[768];
    snprintf(pattern, sizeof(pattern), "%s/*_*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strlen(fd.cFileName) != 21) continue; /* 8+1+8+4 = 21 */
        /* Validate the <addr8>_<crc8>.dll shape explicitly: region_start 0 is
         * LEGAL (the kernel-RAM window starts at phys 0), so a zero parse
         * result can't be used as the invalid sentinel. */
        int valid = (fd.cFileName[8] == '_');
        for (int ci = 0; valid && ci < 8; ci++) {
            char c = fd.cFileName[ci];
            valid = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                    (c >= 'a' && c <= 'f');
        }
        if (!valid) continue;
        uint32_t addr = (uint32_t)strtoul(fd.cFileName, NULL, 16);
        if (s_cache_idx_count >= CACHE_IDX_CAP) break;
        char full[768];
        snprintf(full, sizeof(full), "%s/%s", dir, fd.cFileName);
        if (cache_idx_has_path(full)) continue;  /* rescan idempotence */
        CacheEntry *e = &s_cache_idx[s_cache_idx_count++];
        e->region_start = addr;
        snprintf(e->path, sizeof(e->path), "%s", full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    (void)dir;
#endif
}

/* Scan the namespaced gcc cache (gcc/<arch-abi>/) AND the legacy flat layout
 * (<game_id>/*.dll) for back-compat — existing caches keep loading with no
 * migration. The sljit/<arch-abi>/ namespace is reserved (no on-disk blobs in
 * v1: sljit re-JITs from the coverage manifest; see SLJIT.md §5.3). */
static void scan_cache_dir(void) {
    char dir[768];
    snprintf(dir, sizeof(dir), "%s/%s/gcc/%s",
             s_cache_dir, s_game_id, PSX_OVERLAY_ARCH_ABI);
    scan_one_cache_dir(dir);
    /* Legacy flat layout (pre-namespacing). */
    snprintf(dir, sizeof(dir), "%s/%s", s_cache_dir, s_game_id);
    scan_one_cache_dir(dir);
}

/* True if the cache holds a DLL for this region compiled from an image with
 * this CRC (filename <addr8>_<crc8>.dll). Autocapture's "unseen" test. */
int overlay_loader_has_cached_crc(uint32_t region_start, uint32_t crc) {
    for (int i = 0; i < s_cache_idx_count; i++) {
        if (s_cache_idx[i].region_start != region_start) continue;
        const char *fn = strrchr(s_cache_idx[i].path, '/');
        fn = fn ? fn + 1 : s_cache_idx[i].path;
        if (strlen(fn) == 21 && (uint32_t)strtoul(fn + 9, NULL, 16) == crc)
            return 1;
    }
    return 0;
}

/* ---- Runtime callbacks wired into overlay DLLs via overlay_init() ------ */

extern void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t ra);
extern void psx_check_interrupts(CPUState *cpu);
extern void gte_execute(CPUState *cpu, uint32_t cmd);
extern void psx_syscall(CPUState *cpu, uint32_t code);
extern void psx_unknown_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys);
extern void debug_server_log_call_entry(uint32_t func_addr);

static OverlayCallbacks s_callbacks;

/* Timing-hypothesis probe: native overlay code calls psx_check_interrupts at
 * EVERY block (up to ~100x/function), whereas the dirty-RAM interpreter checks
 * only ~every 4096 instructions + at function exit. That cadence gap can deliver
 * an interrupt at a different point in native vs interp -> divergence with no
 * mistranslation. When s_suppress_irq is set we drop native's per-block checks
 * (cadence ~ interp). If the blue screen vanishes, the cause is interrupt timing.
 * Two modes: full suppress, or rate-limit (call the real check every Nth time).
 *
 * NOTE: this probe rests on the cross-game finding that overlay code never
 * installs its own IRQ/DMA/callback handlers (PsyQ convention — all timing-
 * critical handlers live in resident static code; holds for every sampled
 * title). That makes interrupt-check *cadence* the only native-vs-interp
 * difference. If a future title violates the convention (an overlay installs a
 * handler), this probe is no longer sufficient and a discriminator is needed. */
static int      s_suppress_irq = 0;
static uint32_t s_irq_ratelimit = 0;   /* 0 = full suppress; N = every Nth call */
static uint32_t s_irq_callcount = 0;
static uint64_t s_irq_suppressed = 0;

void overlay_loader_set_irq_suppress(int mode, uint32_t ratelimit) {
    s_suppress_irq  = mode ? 1 : 0;
    s_irq_ratelimit = ratelimit;
    /* Reset counters on every (re)arm so an A/B run reports a clean, isolated
     * suppressed-count for that arming rather than a cumulative total. */
    s_irq_callcount  = 0;
    s_irq_suppressed = 0;
}
void overlay_loader_get_irq_suppress(int *mode, uint32_t *rl, uint64_t *supp) {
    if (mode) *mode = s_suppress_irq;
    if (rl)   *rl   = s_irq_ratelimit;
    if (supp) *supp = s_irq_suppressed;
}

static void overlay_ci_wrapper(CPUState *cpu) {
    if (s_suppress_irq) {
        if (s_irq_ratelimit == 0) { s_irq_suppressed++; return; }
        if ((++s_irq_callcount % s_irq_ratelimit) != 0) { s_irq_suppressed++; return; }
    }
    psx_check_interrupts(cpu);
}

static void init_callbacks(void) {
    extern void psx_restore_state_escape(void);
    s_callbacks.dispatch_call        = psx_dispatch_call;
    s_callbacks.check_interrupts     = overlay_ci_wrapper;
    s_callbacks.gte_execute          = gte_execute;
    s_callbacks.psx_syscall          = psx_syscall;
    s_callbacks.psx_unknown_dispatch = psx_unknown_dispatch;
    s_callbacks.log_call_entry       = debug_server_log_call_entry;
    s_callbacks.psx_restore_state_escape = psx_restore_state_escape;
    /* Call-contract state (ABI v2): DLL code shares the runtime's bail
     * flag and counters through these pointers. */
    s_callbacks.call_bail_flag = &g_psx_call_bail;
    s_callbacks.bail_first     = &g_psx_bail_first;
    s_callbacks.bail_resolved  = &g_psx_bail_resolved;
}

/* ---- DLL loading and export enumeration -------------------------------- */

#ifdef _WIN32
static int load_overlay_dll(const char *dll_path, ManFn *man, int man_n, int dll) {
    HMODULE h = LoadLibraryA(dll_path);
    if (!h) {
        loader_log("LoadLibrary(%s) failed: %lu", dll_path, GetLastError());
        return 0;
    }
    /* ABI gate: reject any DLL whose contract ABI doesn't match this
     * runtime (pre-versioning DLLs lack the export entirely).  Delete the
     * stale file so the autocompile path regenerates it with the current
     * emitter. */
    typedef int (*AbiFn)(void);
    AbiFn abi_fn = (AbiFn)GetProcAddress(h, "overlay_abi");
    int abi = abi_fn ? abi_fn() : 0;
    /* Tag = ABI version (low 16) | codegen flavor (high 16). Mismatch on either
     * (wrong ABI, or a different-flavor cache e.g. widescreen vs base) is
     * rejected + deleted so autocompile regenerates it for THIS build. */
    if (abi != PSX_OVERLAY_ABI_TAG) {
        loader_log("ABI/flavor mismatch in %s: dll=0x%X runtime=0x%X — rejecting "
                   "and deleting stale cache entry", dll_path, abi,
                   PSX_OVERLAY_ABI_TAG);
        FreeLibrary(h);
        DeleteFileA(dll_path);
        return 0;
    }
    typedef void (*InitFn)(const OverlayCallbacks *);
    InitFn init_fn = (InitFn)GetProcAddress(h, "overlay_init");
    if (!init_fn) {
        loader_log("no overlay_init in %s", dll_path);
        FreeLibrary(h);
        return 0;
    }
    init_fn(&s_callbacks);

    BYTE *base = (BYTE *)h;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *exp_dd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exp_dd->VirtualAddress) {
        loader_log("no export dir in %s", dll_path);
        FreeLibrary(h);
        return 0;
    }
    IMAGE_EXPORT_DIRECTORY *exp =
        (IMAGE_EXPORT_DIRECTORY *)(base + exp_dd->VirtualAddress);
    DWORD *names    = (DWORD *)(base + exp->AddressOfNames);
    WORD  *ordinals = (WORD  *)(base + exp->AddressOfNameOrdinals);
    DWORD *funcs    = (DWORD *)(base + exp->AddressOfFunctions);

    int registered = 0;
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char *name = (const char *)(base + names[i]);
        if (strncmp(name, "func_", 5) != 0) continue;
        if (strlen(name) != 13) continue;
        uint32_t addr = (uint32_t)strtoul(name + 5, NULL, 16);
        if (addr == 0) continue;

        WORD ord = ordinals[i];
        OverlayFn fn = (OverlayFn)(base + funcs[ord]);

        /* Precision-first (§1): only register a candidate when the recompiler
         * gave us its code ranges. A function with no manifest entry is left to
         * the interpreter rather than registered with a guessed extent. */
        ManFn *m = man_find(man, man_n, addr);
        if (!m || m->n == 0) { s_no_manifest++; continue; }
        cand_register(addr & 0x1FFFFFFFu, fn, m, dll);
        registered++;
    }
    loader_log("loaded %s -> %d candidates (%u no-manifest)",
               dll_path, registered, s_no_manifest);
    return registered;
}
#else
static int load_overlay_dll(const char *dll_path, ManFn *man, int man_n, int dll) {
    (void)man; (void)man_n; (void)dll;
    void *h = dlopen(dll_path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { loader_log("dlopen(%s) failed: %s", dll_path, dlerror()); return 0; }
    /* ABI gate (see the _WIN32 branch). */
    typedef int (*AbiFn)(void);
    AbiFn abi_fn = (AbiFn)dlsym(h, "overlay_abi");
    int abi = abi_fn ? abi_fn() : 0;
    if (abi != PSX_OVERLAY_ABI_TAG) {
        loader_log("ABI/flavor mismatch in %s: dll=0x%X runtime=0x%X — rejecting "
                   "and deleting stale cache entry", dll_path, abi,
                   PSX_OVERLAY_ABI_TAG);
        dlclose(h);
        remove(dll_path);
        return 0;
    }
    typedef void (*InitFn)(const OverlayCallbacks *);
    InitFn init_fn = (InitFn)dlsym(h, "overlay_init");
    if (!init_fn) { loader_log("no overlay_init in %s", dll_path); dlclose(h); return 0; }
    init_fn(&s_callbacks);
    loader_log("%s loaded (posix export scan TODO)", dll_path);
    return 0;
}
#endif

/* ---- Public API -------------------------------------------------------- */

void overlay_loader_init(const char *cache_dir, const char *game_id) {
    strncpy(s_cache_dir, cache_dir, sizeof(s_cache_dir) - 1);
    strncpy(s_game_id,   game_id,   sizeof(s_game_id)   - 1);
    init_callbacks();
    scan_cache_dir();
    s_active = 1;
}

void overlay_loader_check_cache(uint32_t load_addr, uint32_t size,
                                const uint8_t *bytes) {
    /* DLL loading is deferred to the first dispatch miss (try_load_region). */
    (void)load_addr; (void)size; (void)bytes;
}

/* ---- Lazy region cache check (first dispatch miss for a region) -------- */

#define MAX_CHECKED 64
static uint32_t s_checked[MAX_CHECKED];
static int      s_nchecked = 0;
static int      s_last_file_found = 0;

static int already_checked(uint32_t region_start) {
    for (int i = 0; i < s_nchecked; i++)
        if (s_checked[i] == region_start) return 1;
    return 0;
}
static void mark_checked(uint32_t region_start) {
    if (s_nchecked < MAX_CHECKED) s_checked[s_nchecked++] = region_start;
}

/* Re-scan the cache dir for DLLs compiled after init (step 2.8 autocompile)
 * and clear the checked-regions memo so the next dispatch into a window
 * region reconsiders the cache. Already-loaded DLLs stay loaded;
 * dll_already_loaded() makes the re-walk idempotent. */
void overlay_loader_rescan(void) {
    if (!s_active) return;
    scan_cache_dir();
    s_nchecked = 0;
}

/* Loaded-DLL set — the cache is ADDITIVE: a memory slot reused by several
 * overlays (Tomba's village and overworld both at 0x800E7xxx) has one cached
 * DLL per distinct overlay (keyed by content crc in the filename). We load
 * ALL of them; each contributes its functions as separate candidates, and
 * per-entry validity (the live-RAM hash) decides which candidate is callable
 * at any moment. Nothing is ever clobbered — discoveries accumulate. */
#define MAX_LOADED_DLLS 128
static char s_loaded_paths[MAX_LOADED_DLLS][768];
static int  s_nloaded_paths = 0;

static int dll_already_loaded(const char *path) {
    for (int i = 0; i < s_nloaded_paths; i++)
        if (strcmp(s_loaded_paths[i], path) == 0) return 1;
    return 0;
}

static int load_one_dll(const char *dll_path) {
    /* Sibling code-range manifest: {base}_{crc}.ranges next to the DLL. */
    char ranges_path[800];
    snprintf(ranges_path, sizeof(ranges_path), "%s", dll_path);
    size_t plen = strlen(ranges_path);
    if (plen >= 4 && strcmp(ranges_path + plen - 4, ".dll") == 0)
        snprintf(ranges_path + plen - 4, sizeof(ranges_path) - (plen - 4), ".ranges");

    int man_n = 0;
    ManFn *man = parse_manifest(ranges_path, &man_n);
    if (!man || man_n == 0) {
        loader_log("no/empty manifest %s — DLL left to interpreter", ranges_path);
        free(man);
        return 0;
    }
    int registered = load_overlay_dll(dll_path, man, man_n, s_ndlls);
    free(man);
    if (registered <= 0) return 0;

    if (s_nloaded_paths < MAX_LOADED_DLLS) {
        strncpy(s_loaded_paths[s_nloaded_paths], dll_path, 767);
        s_loaded_paths[s_nloaded_paths][767] = '\0';
        s_nloaded_paths++;
    }
    s_ndlls++;
    return registered;
}

static void try_load_region(uint32_t phys) {
    extern uint32_t dirty_ram_get_bitmap_word(uint32_t word_index);

    uint32_t page_sz = 4096u;

    /* Walk back over the contiguous dirty run to recover region_start — cache
     * DLLs are keyed by this start address in their filename. */
    uint32_t pg = phys / page_sz;
    while (pg > 0) {
        uint32_t pp = pg - 1;
        if (!((dirty_ram_get_bitmap_word(pp >> 5) >> (pp & 31u)) & 1u)) break;
        pg = pp;
    }
    uint32_t region_start = pg * page_sz;

    if (already_checked(region_start)) return;
    mark_checked(region_start);

    /* Load EVERY cached DLL for this region_start (additive / multi-candidate),
     * not just the first. */
    for (int ci = 0; ci < s_cache_idx_count; ci++) {
        if (s_cache_idx[ci].region_start != region_start) continue;
        if (dll_already_loaded(s_cache_idx[ci].path)) continue;
        s_last_file_found = 1;
        load_one_dll(s_cache_idx[ci].path);
    }
}

/* ---- Dispatch ---------------------------------------------------------- */

int overlay_loader_dispatch(CPUState *cpu, uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    int head = idx_head(phys);
    if (head < 0 && s_active && overlay_cache_window_contains(phys)) {
        try_load_region(phys);
        head = idx_head(phys);
        /* sljit JIT-on-miss (SLJIT.md §7 step 5), VALIDATION-GATED: only when
         * the active backend is sljit AND the same-state differential is armed,
         * and never inside a shadow run. This keeps normal play byte-identical
         * (no sljit shard is ever created unless explicitly validating) while
         * letting run_shadow_diff exercise fresh shards against the interpreter.
         * Live (non-diff) sljit execution stays disabled until the gate passes. */
        if (head < 0 && s_diff_mode && !s_in_shadow &&
            overlay_backend_active() == OVERLAY_BACKEND_SLJIT) {
            try_sljit_region(addr);   /* virtual entry (carries KSEG bits) */
            head = idx_head(phys);
        }
    }

    for (int i = head; i >= 0; i = s_cand[i].next) {
        Candidate *c = &s_cand[i];
        if (c->state == ENTRY_BLACKLIST) continue;

        /* CORRECTNESS-FIRST — parity (native == interpreter) is not yet proven,
         * so we verify the live code bytes match what we compiled on EVERY
         * dispatch, immediately before running native. This is the safe stance
         * AND the decisive test: if a stale-native blue screen still occurs when
         * we PROVABLY only run native on byte-exact matches, the fault is the
         * native translation itself (parity), not the validity tracking. The
         * generation counter is retained only as a diagnostic (cand_gensum). */
        uint32_t live = cand_crc(c);
        s_rehashes++;
        s_last_crc = live;
        c->val_gen = cand_gensum(c);
        if (live == c->crc_code) {
            if (c->state != ENTRY_VALID) {
                c->state = ENTRY_VALID;
                s_revalidations++;              /* reload-on-return */
                s_valid_count++;
            }
            /* Same-state differential: run native+interp from identical state,
             * compare, keep the interp result. Takes precedence over the A/B
             * toggle. */
            if (s_diff_mode && !s_in_shadow) { run_shadow_diff(cpu, c, addr); return 1; }

            /* A/B: prove whether native EXECUTION is the cause. When off, the
             * candidate matched (byte-exact) but we DON'T run native — interp
             * handles it. */
            if (!s_native_exec) { s_would_run_native++; s_disp_interp++; return 0; }

            /* Record into the always-on ring BEFORE the call; mark in-progress
             * so a freeze inside this fn is visible at dump time. */
            uint32_t slot = s_nring_pos++ & (NRING_CAP - 1u);
            s_nring[slot].addr = c->addr;
            s_nring[slot].crc  = c->crc_code;
            s_nring[slot].seq  = ++s_nring_seq;
            s_nring[slot].returned = 0;
            uint32_t prev_inprogress = s_native_inprogress;
            s_native_inprogress = c->addr;
            s_native_calls_total++;

            if (s_active_depth < (int)(sizeof(s_active_stack) / sizeof(s_active_stack[0])))
                s_active_stack[s_active_depth++] = i;
            s_disp_native++;
            /* Delimit this native execution in the interp insn ring (native code
             * emits no per-insn entries; markers keep the timeline alignable). */
            extern void dirty_ram_log_marker(uint32_t addr, uint32_t tag, int kind);
            uint32_t mtag = (uint32_t)s_nring[slot].seq;  /* stable across nesting */
            dirty_ram_log_marker(c->addr, mtag, 0);
            c->fn(cpu);
            dirty_ram_log_marker(c->addr, mtag, 1);
            if (s_active_depth > 0) s_active_depth--;

            s_nring[slot].returned = 1;
            s_native_inprogress = prev_inprogress;   /* restore (nested calls) */
            return 1;
        } else {
            s_rehash_miss++;
            if (c->state == ENTRY_VALID) {
                c->state = ENTRY_INVALID;
                s_invalidations++;
                if (s_valid_count > 0) s_valid_count--;
            } else {
                c->state = ENTRY_INVALID;
            }
            s_stale_blocked++;
        }
    }

    s_disp_interp++;
    return 0;
}

/* ---- Self-modification of an actively-executing entry (§8.5) ------------ */
/* Lazy re-hash on the NEXT dispatch is too late if a native function modifies
 * its own code and continues executing the modified bytes within the same
 * activation (native runs the originally-compiled semantics). We can't recover
 * the current activation, so we permanently demote that entry to interp. Called
 * from memory.c only when the written page is watched. */
void overlay_loader_active_write_check(uint32_t phys, uint32_t size) {
    extern uint32_t g_debug_last_store_pc;
    uint32_t p = phys & 0x1FFFFFFFu;
    for (int d = 0; d < s_active_depth; d++) {
        Candidate *c = &s_cand[s_active_stack[d]];
        for (int i = 0; i < c->nranges; i++) {
            uint32_t lo = c->range_lo[i];
            uint32_t hi = lo + c->range_len[i];
            if (p < hi && p + size > lo) {
                if (c->state != ENTRY_BLACKLIST) {
                    c->state = ENTRY_BLACKLIST;
                    s_selfmod++;
                    if (s_valid_count > 0) s_valid_count--;
                    s_last_write_pc   = g_debug_last_store_pc;
                    s_last_write_addr = phys;
                    s_last_write_size = size;
                    loader_log("blacklist self-mod entry 0x%08X (write 0x%08X)",
                               c->addr, phys);
                }
                break;
            }
        }
    }
}

/* ---- Status getters (signatures preserved for debug_server.c) ---------- */

void overlay_loader_get_counters(uint32_t *loads, uint32_t *invalidations,
                                 uint32_t *unregistered,
                                 uint64_t *disp_native, uint64_t *disp_interp,
                                 uint64_t *stale_blocked,
                                 uint32_t *last_write_pc,
                                 uint32_t *last_write_addr,
                                 uint32_t *last_write_size,
                                 int *regions, uint32_t *revalidations) {
    if (loads)           *loads           = (uint32_t)s_ndlls;
    if (invalidations)   *invalidations   = s_invalidations;
    if (unregistered)    *unregistered    = s_no_manifest;
    if (disp_native)     *disp_native     = s_disp_native;
    if (disp_interp)     *disp_interp     = s_disp_interp;
    if (stale_blocked)   *stale_blocked   = s_stale_blocked;
    if (last_write_pc)   *last_write_pc   = s_last_write_pc;
    if (last_write_addr) *last_write_addr = s_last_write_addr;
    if (last_write_size) *last_write_size = s_last_write_size;
    if (regions)         *regions         = s_ndlls;
    if (revalidations)   *revalidations   = s_revalidations;
}

/* Reload diagnostics. Repurposed for the per-entry model:
 *   r0_valid       -> candidates currently VALID
 *   r0_writes...   -> entries blacklisted (self-mod)
 *   r0_fn_lo       -> total candidates registered
 *   r0_fn_hi       -> DLLs loaded
 *   r0_crc_live    -> last computed code-range crc
 *   reval_attempts -> code-range hashes computed
 *   reval_crc_miss -> hashes that did not match
 *   last_reval_crc -> last computed crc                                    */
void overlay_loader_get_reload_debug(int *r0_valid, uint32_t *r0_writes,
                                     uint32_t *r0_fn_lo, uint32_t *r0_fn_hi,
                                     uint32_t *r0_crc_live,
                                     uint32_t *reval_attempts,
                                     uint32_t *reval_crc_miss,
                                     uint32_t *last_reval_crc) {
    if (r0_valid)       *r0_valid       = s_valid_count;
    if (r0_writes)      *r0_writes      = s_selfmod;
    if (r0_fn_lo)       *r0_fn_lo       = (uint32_t)s_cand_n;
    if (r0_fn_hi)       *r0_fn_hi       = (uint32_t)s_ndlls;
    if (r0_crc_live)    *r0_crc_live    = s_last_crc;
    if (reval_attempts) *reval_attempts = s_rehashes;
    if (reval_crc_miss) *reval_crc_miss = s_rehash_miss;
    if (last_reval_crc) *last_reval_crc = s_last_crc;
}

int overlay_loader_registered_count(void) { return s_valid_count; }

/* sljit Tier-2 shards registered as candidates this session (diagnostics). */
uint32_t overlay_loader_sljit_registered(void) { return s_sljit_registered; }

/* Force a one-shot sljit JIT attempt of the leaf function at `addr` from live
 * RAM and register it on success (bypasses the diff-mode gate — a probe). For
 * the sljit_try debug command. Returns via the result struct. */
void overlay_loader_sljit_probe(uint32_t addr, OverlaySljitResult *out) {
    out->fn = NULL; out->code_lo = 0; out->code_len = 0; out->insns = 0;
    uint8_t *ram = memory_get_ram_ptr();
    if (!ram) return;
    /* Virtual entry so return_pc / jal targets carry the KSEG bits (see
     * try_sljit_region); byte offset is still (entry & 0x1FFFFFFF) = phys. */
    overlay_sljit_try_compile(addr, ram, 2u * 1024u * 1024u, 0u, out);
    if (out->fn)
        register_sljit_candidate(addr & 0x1FFFFFFFu, (OverlayFn)out->fn,
                                 out->code_lo, out->code_len);
}

/* ---- Native↔interp execution fingerprint differential (§5-E) ----------- */
/* For each CANDIDATE function execution we record the FULL register file at
 * entry and exit (plus the guest cycle), tagged native vs interp. Run once
 * native-OFF (all candidates interpreted = oracle) and once native-ON; diff by
 * sequence — the first entry whose in-state differs names the exact register
 * AND value where the two trajectories part ways, and the cycle field
 * quantifies native↔interp cycle-accounting skew over the aligned prefix.
 * Logging is purely additive (no control-flow change), driven from the single
 * dirty_ram_dispatch chokepoint. */
typedef struct {
    uint64_t seq;
    uint64_t cycle;          /* guest cycle at exit (log time)                 */
    uint32_t addr;
    uint32_t in_crc, out_crc;
    int      native;
    uint32_t in_regs[34];    /* r0..r31, hi, lo at entry                       */
    uint32_t out_regs[34];   /* r0..r31, hi, lo at exit                        */
} FpEnt;
#define FP_CAP (1u << 16)   /* ~19 MB with full reg files; ~65K executions     */
static FpEnt    s_fp[FP_CAP];
static uint64_t s_fp_seq = 0;

int overlay_loader_is_candidate(uint32_t phys) {
    return idx_head(phys & 0x1FFFFFFFu) >= 0;
}

/* ---- Same-state native↔interp differential (confident measurement) ------ */
/* At a matched dispatch: snapshot CPU+RAM, run native (discard), restore, run
 * interpreter (KEEP — game stays correct), compare under IDENTICAL input state.
 * Eliminates manual-nav desync. Interrupts suppressed during both shadow runs
 * so the comparison isolates COMPUTATION (and is longjmp-safe). A divergence
 * here = a real codegen bug (function + exact register/RAM). Zero divergence =
 * computation is correct and the fault is timing/interrupt-ordering. */
#define SHADOW_RAM_SIZE  (2u * 1024u * 1024u)
#define SHADOW_SPAD_SIZE 1024u
static uint8_t  s_ram0[SHADOW_RAM_SIZE];   /* pre-call main-RAM snapshot  */
static uint8_t  s_ramN[SHADOW_RAM_SIZE];   /* post-native main-RAM        */
static uint8_t  s_spad0[SHADOW_SPAD_SIZE]; /* pre-call scratchpad snapshot*/
/* s_diff_mode / s_in_shadow declared above (before dispatch). */

typedef struct {
    uint64_t seq; uint32_t addr;
    int      reg;          /* first differing gpr index, -1 none           */
    uint32_t reg_native, reg_interp;
    int      hi_diff, lo_diff;
    int64_t  ram_off;      /* first differing RAM byte, -1 none            */
    uint32_t ram_native, ram_interp;  /* the differing word               */
} ShadowDiv;
#define SDIV_CAP 512
static ShadowDiv s_sdiv[SDIV_CAP];
static int       s_sdiv_n = 0;
static uint64_t  s_shadow_calls = 0;
static uint64_t  s_shadow_divs  = 0;

/* One-shot full-state capture of the FIRST divergence: complete native and
 * interp register files so the diverging path can be localized. */
static int      s_detail_captured = 0;
static uint32_t s_detail_addr = 0;
static uint32_t s_detail_nat_gpr[32], s_detail_int_gpr[32];
static uint32_t s_detail_nat_hi, s_detail_nat_lo, s_detail_int_hi, s_detail_int_lo;

void overlay_loader_set_diff_mode(int on) { s_diff_mode = on ? 1 : 0; }

static void run_shadow_diff(CPUState *cpu, Candidate *c, uint32_t addr) {
    extern uint8_t *memory_get_ram_ptr(void);
    extern uint8_t *memory_get_scratchpad_ptr(void);
    extern int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr);
    uint8_t *ram  = memory_get_ram_ptr();
    uint8_t *spad = memory_get_scratchpad_ptr();

    s_in_shadow = 1;
    int saved_supp = s_suppress_irq;
    s_suppress_irq = 1;                 /* isolate computation; longjmp-safe */

    CPUState cpu0 = *cpu;
    memcpy(s_ram0,  ram,  SHADOW_RAM_SIZE);
    memcpy(s_spad0, spad, SHADOW_SPAD_SIZE);

    /* Native (discarded). Nested calls run native (native_exec on). */
    c->fn(cpu);
    CPUState cpuN = *cpu;
    memcpy(s_ramN, ram, SHADOW_RAM_SIZE);

    /* Restore CPU + main RAM + scratchpad, then run interpreter (kept). Restoring
     * scratchpad is essential — these functions use it as working memory, and
     * leaving native's mutations there would make the interp pass diverge
     * spuriously. (Device/DMA/cycle state still isn't restored; functions that
     * read those may still show as divergent — treated as suspect, not proof.) */
    *cpu = cpu0;
    memcpy(ram,  s_ram0,  SHADOW_RAM_SIZE);
    memcpy(spad, s_spad0, SHADOW_SPAD_SIZE);
    int sv = s_native_exec;
    s_native_exec = 0;
    /* Shadow run executes the candidate to its return: the entry $ra is the
     * stop contract, same as the live dispatch path. */
    dirty_ram_dispatch(cpu, addr, cpu->gpr[31]);  /* runs interp (guarded) */
    s_native_exec = sv;

    s_shadow_calls++;

    /* Compare native (cpuN/s_ramN) vs interp (*cpu/ram) under identical input. */
    int reg = -1;
    for (int r = 1; r < 32; r++) if (cpuN.gpr[r] != cpu->gpr[r]) { reg = r; break; }
    int hidiff = (cpuN.hi != cpu->hi), lodiff = (cpuN.lo != cpu->lo);
    int64_t ramoff = -1;
    if (memcmp(s_ramN, ram, SHADOW_RAM_SIZE) != 0) {
        for (uint32_t a = 0; a < SHADOW_RAM_SIZE; a++)
            if (s_ramN[a] != ram[a]) { ramoff = (int64_t)a; break; }
    }
    if (reg >= 0 || hidiff || lodiff || ramoff >= 0) {
        s_shadow_divs++;
        if (!s_detail_captured) {
            s_detail_captured = 1;
            s_detail_addr = c->addr;
            for (int r = 0; r < 32; r++) {
                s_detail_nat_gpr[r] = cpuN.gpr[r];
                s_detail_int_gpr[r] = cpu->gpr[r];
            }
            s_detail_nat_hi = cpuN.hi; s_detail_nat_lo = cpuN.lo;
            s_detail_int_hi = cpu->hi; s_detail_int_lo = cpu->lo;
        }
        if (s_sdiv_n < SDIV_CAP) {
            ShadowDiv *d = &s_sdiv[s_sdiv_n++];
            d->seq = s_shadow_calls; d->addr = c->addr;
            d->reg = reg;
            d->reg_native = (reg >= 0) ? cpuN.gpr[reg] : 0;
            d->reg_interp = (reg >= 0) ? cpu->gpr[reg] : 0;
            d->hi_diff = hidiff; d->lo_diff = lodiff;
            d->ram_off = ramoff;
            if (ramoff >= 0) {
                uint32_t a = (uint32_t)ramoff & ~3u;
                d->ram_native = *(uint32_t *)&s_ramN[a];
                d->ram_interp = *(uint32_t *)&ram[a];
            }
        }
    }
    s_suppress_irq = saved_supp;
    s_in_shadow = 0;
}

int overlay_loader_dump_shadow_detail(char *out, int cap) {
    int n = 0;
    n += snprintf(out + n, cap - n,
        "{\"captured\":%d,\"addr\":\"0x%08X\",\"regs\":[", s_detail_captured, s_detail_addr);
    static const char *rn[32] = {"zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7","s0","s1","s2","s3","s4","s5",
        "s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
    int first = 1;
    for (int r = 0; r < 32; r++) {
        if (s_detail_nat_gpr[r] == s_detail_int_gpr[r]) continue;
        n += snprintf(out + n, cap - n,
            "%s{\"r\":%d,\"name\":\"%s\",\"native\":\"0x%08X\",\"interp\":\"0x%08X\"}",
            first ? "" : ",", r, rn[r], s_detail_nat_gpr[r], s_detail_int_gpr[r]);
        first = 0;
    }
    n += snprintf(out + n, cap - n, "],\"hi\":{\"native\":\"0x%08X\",\"interp\":\"0x%08X\"},"
        "\"lo\":{\"native\":\"0x%08X\",\"interp\":\"0x%08X\"}}",
        s_detail_nat_hi, s_detail_int_hi, s_detail_nat_lo, s_detail_int_lo);
    return n;
}

int overlay_loader_dump_shadow(char *out, int cap) {
    int n = 0;
    n += snprintf(out + n, cap - n,
        "{\"diff_mode\":%d,\"shadow_calls\":%llu,\"divergences\":%llu,\"records\":[",
        s_diff_mode, (unsigned long long)s_shadow_calls,
        (unsigned long long)s_shadow_divs);
    for (int i = 0; i < s_sdiv_n && n < cap - 200; i++) {
        ShadowDiv *d = &s_sdiv[i];
        n += snprintf(out + n, cap - n,
            "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"reg\":%d,"
            "\"reg_native\":\"0x%08X\",\"reg_interp\":\"0x%08X\","
            "\"hi\":%d,\"lo\":%d,\"ram_off\":%lld,"
            "\"ram_native\":\"0x%08X\",\"ram_interp\":\"0x%08X\"}",
            i ? "," : "", (unsigned long long)d->seq, d->addr, d->reg,
            d->reg_native, d->reg_interp, d->hi_diff, d->lo_diff,
            (long long)d->ram_off, d->ram_native, d->ram_interp);
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

/* Fingerprint over the general registers (r1..r31) + hi/lo. r0 excluded
 * (always 0). pc excluded (the return target is trivially equal at exit). */
uint32_t overlay_regs_crc(const CPUState *cpu) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t *)&cpu->gpr[1], sizeof(uint32_t) * 31);
    crc = crc32_update(crc, (const uint8_t *)&cpu->hi, sizeof(uint32_t));
    crc = crc32_update(crc, (const uint8_t *)&cpu->lo, sizeof(uint32_t));
    return crc ^ 0xFFFFFFFFu;
}

/* Snapshot r0..r31 + hi/lo into a 34-word buffer (entry-state capture). */
void overlay_regs_snap(uint32_t out[34], const CPUState *cpu) {
    memcpy(out, cpu->gpr, sizeof(uint32_t) * 32);
    out[32] = cpu->hi;
    out[33] = cpu->lo;
}

/* CRC over words 1..33 (r0 excluded — always 0), matching overlay_regs_crc. */
static uint32_t regs34_crc(const uint32_t *r) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t *)&r[1], sizeof(uint32_t) * 33);
    return crc ^ 0xFFFFFFFFu;
}

void overlay_fp_log(uint32_t addr, const uint32_t *in_regs,
                    const CPUState *cpu, int native) {
    extern uint64_t psx_get_cycle_count(void);
    uint64_t s = s_fp_seq++;
    FpEnt *e = &s_fp[s & (FP_CAP - 1u)];
    e->seq = s; e->cycle = psx_get_cycle_count();
    e->addr = addr & 0x1FFFFFFFu; e->native = native;
    memcpy(e->in_regs, in_regs, sizeof(e->in_regs));
    overlay_regs_snap(e->out_regs, cpu);
    e->in_crc  = regs34_crc(e->in_regs);
    e->out_crc = regs34_crc(e->out_regs);
}

/* Execute `addr` natively if a validated overlay candidate exists, keeping the
 * §5-E fingerprint record (same as the dirty_ram_dispatch chokepoint). Called
 * from the dirty-RAM interpreter's jal/jalr handlers so native overlay callees
 * get the SAME call contract as statically-compiled callees: execute as a
 * unit, then the interpreter resumes at the call's return address. Without
 * this, the call surfaces to the dispatch loop as a bare pc value; the native
 * callee's C-style return (pc==0) then unwinds the loop past the suspended
 * caller continuation — the caller's epilogue never runs and its stack frame
 * leaks (root cause of the dwarf->overworld native blue screen).
 * Returns 1 iff a native candidate ran. */
int overlay_loader_call_native(CPUState *cpu, uint32_t addr) {
    if (!s_native_exec) return 0;  /* interp mode: keep the legacy inline path */
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (idx_head(phys) < 0) return 0;        /* fast reject: not a candidate */
    uint32_t in_regs[34];
    overlay_regs_snap(in_regs, cpu);
    if (!overlay_loader_dispatch(cpu, addr)) return 0;
    overlay_fp_log(addr, in_regs, cpu, 1);
    return 1;
}

/* ---- jal/jalr call helper for sljit shards (SLJIT.md §7) ---------------- */
/* A JIT'd shard calls this at every jal/jalr site instead of open-coding the
 * call contract. It reproduces the dirty-RAM interpreter's call path EXACTLY
 * (see exec_one cases 0x03 / SPECIAL 0x09 and dispatch_nonlocal_call), so a
 * shard-issued call behaves identically to an interpreted one — including the
 * wild-return / bail unwind that the contract guards (Bug A/C/D family). The
 * one principled difference from the interpreter's local-dirty fast path: a
 * native shard cannot resume itself block-by-block, so a not-yet-native callee
 * is run as a UNIT via psx_dispatch_call (which handles interp/dirty callees to
 * the return contract) rather than the interpreter's pc-chain. See overlay_sljit.h. */
int psx_sljit_call(CPUState *cpu, uint32_t target, uint32_t return_pc,
                   int check_contract) {
    uint32_t site_sp = cpu->gpr[29];   /* sp at the call (after the delay slot) */
#ifdef PSX_HAS_GAME_DISPATCH
    {
        extern int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr);
        cpu->pc = 0;
        if (psx_dispatch_game_compiled(cpu, target)) {
            if (g_psx_call_bail) return 1;
            if (cpu->pc != 0) return 1;
            if (check_contract && psx_call_contract(cpu, return_pc, site_sp)) return 1;
            return 0;
        }
    }
#endif
    cpu->pc = 0;
    if (overlay_loader_call_native(cpu, target)) {
        if (g_psx_call_bail) return 1;
        if (cpu->pc != 0) return 1;
        if (check_contract && psx_call_contract(cpu, return_pc, site_sp)) return 1;
        return 0;
    }
    /* Not a resolved compiled/native unit: dispatch the callee as a unit (the
     * interpreter's nonlocal path — handles interp/dirty callees + the return
     * contract internally). */
    cpu->pc = 0;
    psx_dispatch_call(cpu, target, return_pc);
    if (g_psx_call_bail) return 1;
    if (cpu->pc != 0) return 1;
    return 0;
}

/* Write the whole fingerprint log to a file (no TCP size limit). Returns the
 * number of entries written, or -1 on open failure. */
int overlay_loader_write_fp_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    uint64_t total = s_fp_seq;
    uint64_t start = (total > FP_CAP) ? (total - FP_CAP) : 0;
    fputc('[', f);
    int first = 1, count = 0;
    for (uint64_t s = start; s < total; s++) {
        FpEnt *e = &s_fp[s & (FP_CAP - 1u)];
        fprintf(f,
            "%s{\"seq\":%llu,\"cycle\":%llu,\"addr\":\"0x%08X\",\"in\":\"0x%08X\","
            "\"out\":\"0x%08X\",\"native\":%d,\"in_regs\":[",
            first ? "" : ",\n", (unsigned long long)e->seq,
            (unsigned long long)e->cycle, e->addr,
            e->in_crc, e->out_crc, e->native);
        for (int r = 0; r < 34; r++)
            fprintf(f, "%s\"0x%08X\"", r ? "," : "", e->in_regs[r]);
        fputs("],\"out_regs\":[", f);
        for (int r = 0; r < 34; r++)
            fprintf(f, "%s\"0x%08X\"", r ? "," : "", e->out_regs[r]);
        fputs("]}", f);
        first = 0; count++;
    }
    fputs("]\n", f);
    fclose(f);
    return count;
}

/* Dump the native-call ring (most-recent first) + the in-progress entry. The
 * in_progress field being nonzero means a native function was entered and never
 * returned — a freeze INSIDE native code, pointing straight at the suspect. */
int overlay_loader_dump_native_ring(char *out, int cap) {
    int n = 0;
    n += snprintf(out + n, cap - n,
        "{\"native_exec\":%d,\"calls_total\":%llu,\"would_run\":%llu,"
        "\"in_progress\":\"0x%08X\",\"recent\":[",
        s_native_exec, (unsigned long long)s_native_calls_total,
        (unsigned long long)s_would_run_native, s_native_inprogress);
    /* Walk backward from the most recent up to 64 entries. */
    int shown = 0;
    for (uint32_t k = 0; k < NRING_CAP && shown < 64 && n < cap - 120; k++) {
        uint32_t idx = (s_nring_pos - 1u - k) & (NRING_CAP - 1u);
        if (s_nring[idx].seq == 0) break;
        n += snprintf(out + n, cap - n,
            "%s{\"addr\":\"0x%08X\",\"crc\":\"0x%08X\",\"seq\":%llu,\"returned\":%d}",
            shown ? "," : "", s_nring[idx].addr, s_nring[idx].crc,
            (unsigned long long)s_nring[idx].seq, s_nring[idx].returned);
        shown++;
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

/* Diagnostic: dump every candidate with its stored vs live hash and generation
 * state, so reload behaviour can be inspected directly (Rule 3 — visibility via
 * the debug server, not logs). Writes a JSON array into `out`; returns bytes
 * written. */
int overlay_loader_dump_candidates(char *out, int cap) {
    int n = 0;
    n += snprintf(out + n, cap - n, "[");
    for (int i = 0; i < s_cand_n && n < cap - 160; i++) {
        Candidate *c = &s_cand[i];
        uint32_t live = cand_crc(c);
        uint32_t sum  = cand_gensum(c);
        n += snprintf(out + n, cap - n,
            "%s{\"addr\":\"0x%08X\",\"state\":%d,\"nranges\":%d,"
            "\"crc\":\"0x%08X\",\"live\":\"0x%08X\",\"match\":%d,"
            "\"val_gen\":%u,\"gen\":%u,\"dll\":%d}",
            i ? "," : "", c->addr, c->state, c->nranges,
            c->crc_code, live, (live == c->crc_code) ? 1 : 0,
            c->val_gen, sum, c->dll);
    }
    n += snprintf(out + n, cap - n, "]");
    return n;
}

void overlay_loader_get_status(int *active, int *registered,
                               int *regions_checked,
                               char *cache_dir_out, int cache_dir_len,
                               char *game_id_out,   int game_id_len,
                               uint32_t *checked_out, int checked_max,
                               int *checked_written,
                               uint32_t *last_crc_out, int *last_file_found_out) {
    if (active)          *active          = s_active;
    if (registered)      *registered      = s_valid_count;
    if (regions_checked) *regions_checked = s_nchecked;
    if (cache_dir_out)   strncpy(cache_dir_out, s_cache_dir, (size_t)cache_dir_len - 1);
    if (game_id_out)     strncpy(game_id_out,   s_game_id,   (size_t)game_id_len   - 1);
    if (checked_out && checked_written) {
        int n = s_nchecked < checked_max ? s_nchecked : checked_max;
        for (int i = 0; i < n; i++) checked_out[i] = s_checked[i];
        *checked_written = n;
    }
    if (last_crc_out)        *last_crc_out        = s_last_crc;
    if (last_file_found_out) *last_file_found_out = s_last_file_found;
}

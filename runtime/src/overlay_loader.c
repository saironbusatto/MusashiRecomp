#include "overlay_loader.h"
#include "overlay_api.h"
#include "crc32.h"
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

/* ---- Dynamic dispatch hash table --------------------------------------- */
/* Open-addressed hash map: physical addr → compiled function pointer.
 * Sized at 16384 slots — enough for ~10 overlays × ~500 functions each
 * at <50% load factor. */

#define DYNTAB_CAP  16384u
#define DYNTAB_MASK (DYNTAB_CAP - 1u)

typedef void (*OverlayFn)(CPUState *);

typedef struct {
    uint32_t   addr;   /* physical address, 0 = empty */
    OverlayFn  fn;
    int        region; /* index into s_regions, -1 = none */
} DynEntry;

static DynEntry  s_table[DYNTAB_CAP];
static int       s_count = 0;

/* ---- Registered overlay regions (Inc1-D: registration lifetime) --------- */
/* A registration is only callable while the RAM it was compiled from is
 * unchanged.  Each region carries a validity flag; a write into the region's
 * watched pages flips it invalid.  We do NOT delete dyntab entries (messy with
 * open addressing) — lookup consults the region's validity, so an invalidated
 * region's functions become non-callable in O(1).  Invariant:
 * Compiled (DLL loaded) != Registered (in dyntab) != Callable (region valid). */
#define MAX_OVL_REGIONS 32
typedef struct {
    uint32_t base;        /* phys region start */
    uint32_t size;        /* bytes (page-aligned span) */
    uint32_t generation;  /* bumped on each invalidation */
    int      valid;       /* 1 = functions callable */
    int      func_count;  /* functions registered for this region */
    /* Inc2 reload-on-return: re-validate when the SAME overlay loads back.
     * crc_live = content hash of the region when it last registered OK.
     * After invalidation we keep watching and count writes; once the region
     * is substantially rewritten (a real reload, not incidental data writes)
     * we re-hash once and, on a match, flip valid back on — the dyntab entries
     * never left, so this is O(1) and needs no DLL reload. */
    uint32_t crc_live;
    uint32_t writes_since_invalid;
    /* Code span = [fn_lo, fn_hi) covering the registered function entries. We
     * watch and hash only this, NOT the whole dirty region — the overlay writes
     * its own data elsewhere in the region during play, and watching that data
     * would falsely invalidate the (unchanged) code registration. */
    uint32_t fn_lo;
    uint32_t fn_hi;
} OvlRegion;
static OvlRegion s_regions[MAX_OVL_REGIONS];
static int       s_nregions = 0;

/* Counters / last-write info, surfaced via overlay_loader_status. */
static uint32_t s_loads          = 0;
static uint32_t s_invalidations  = 0;
static uint32_t s_unregistered   = 0;
static uint64_t s_disp_native    = 0;
static uint64_t s_disp_interp    = 0;
static uint64_t s_stale_blocked  = 0;
static uint32_t s_revalidations  = 0;
/* Inc2 diagnostics: see exactly where reload-on-return stalls. */
static uint32_t s_reval_attempts = 0;   /* threshold met -> hash computed */
static uint32_t s_reval_crc_miss = 0;   /* hash computed but != crc_live */
static uint32_t s_last_reval_crc = 0;   /* most recent recomputed code-span crc */
static uint32_t s_last_write_pc   = 0;
static uint32_t s_last_write_addr = 0;
static uint32_t s_last_write_size = 0;

static void dyntab_insert(uint32_t phys, OverlayFn fn, int region)
{
    uint32_t h = (phys * 2654435761u) & DYNTAB_MASK;
    uint32_t i;
    for (i = 0; i < DYNTAB_CAP; i++) {
        uint32_t idx = (h + i) & DYNTAB_MASK;
        if (s_table[idx].addr == 0 || s_table[idx].addr == phys) {
            if (s_table[idx].addr == 0) s_count++;
            s_table[idx].addr   = phys;
            s_table[idx].fn     = fn;
            s_table[idx].region = region;
            return;
        }
    }
    /* Table full — shouldn't happen at <50% load */
}

static OverlayFn dyntab_lookup(uint32_t phys)
{
    uint32_t h = (phys * 2654435761u) & DYNTAB_MASK;
    uint32_t i;
    for (i = 0; i < DYNTAB_CAP; i++) {
        uint32_t idx = (h + i) & DYNTAB_MASK;
        if (s_table[idx].addr == 0) return NULL;
        if (s_table[idx].addr == phys) {
            int r = s_table[idx].region;
            /* A registered function whose source region was overwritten is
             * NOT callable — block it and let the interpreter handle the new
             * content (it self-heals via recapture). */
            if (r >= 0 && r < s_nregions && !s_regions[r].valid) {
                s_stale_blocked++;
                return NULL;
            }
            return s_table[idx].fn;
        }
    }
    return NULL;
}

/* ---- Global state ------------------------------------------------------ */

static char s_cache_dir[512];
static char s_game_id[64];
static int  s_active = 0;

/* Rule 3: no stderr logging. The most recent loader event is recorded here
 * and surfaced through the `overlay_loader_status` TCP command instead of
 * being printed. */
static char s_last_msg[256] = {0};

static void loader_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_msg, sizeof(s_last_msg), fmt, ap);
    va_end(ap);
}

const char *overlay_loader_last_msg(void) { return s_last_msg; }

/* ---- Cache index: region_start → dll path ------------------------------- */
/* Scanned once at init from {cache_dir}/{game_id}/{8hex}_{8hex}.dll files.
 * Avoids recomputing the DMA CRC at dispatch time (which fails after
 * fast_boot because DMA callbacks haven't fired to fill s_entries). */

#define CACHE_IDX_CAP 256
typedef struct { uint32_t region_start; char path[768]; } CacheEntry;
static CacheEntry s_cache_idx[CACHE_IDX_CAP];
static int        s_cache_idx_count = 0;

static void scan_cache_dir(void)
{
#ifdef _WIN32
    char pattern[768];
    snprintf(pattern, sizeof(pattern), "%s/%s/*_*.dll",
             s_cache_dir, s_game_id);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strlen(fd.cFileName) != 21) continue; /* 8+1+8+4 = 21 */
        uint32_t addr = (uint32_t)strtoul(fd.cFileName, NULL, 16);
        if (addr == 0) continue;
        if (s_cache_idx_count >= CACHE_IDX_CAP) break;
        CacheEntry *e = &s_cache_idx[s_cache_idx_count++];
        e->region_start = addr;
        snprintf(e->path, sizeof(e->path), "%s/%s/%s",
                 s_cache_dir, s_game_id, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
}

/* ---- Runtime callbacks wired into overlay DLLs via overlay_init() ------ */

extern void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t ra);
extern void psx_check_interrupts(CPUState *cpu);
extern void gte_execute(CPUState *cpu, uint32_t cmd);
extern void psx_syscall(CPUState *cpu, uint32_t code);
extern void psx_unknown_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys);
extern void debug_server_log_call_entry(uint32_t func_addr);

static OverlayCallbacks s_callbacks;

static void init_callbacks(void)
{
    s_callbacks.dispatch_call       = psx_dispatch_call;
    s_callbacks.check_interrupts    = psx_check_interrupts;
    s_callbacks.gte_execute         = gte_execute;
    s_callbacks.psx_syscall         = psx_syscall;
    s_callbacks.psx_unknown_dispatch = psx_unknown_dispatch;
    s_callbacks.log_call_entry      = debug_server_log_call_entry;
}

/* ---- DLL loading and export enumeration -------------------------------- */

#ifdef _WIN32
static int load_overlay_dll(const char *dll_path, uint32_t load_addr_virt, int region)
{
    HMODULE dll = LoadLibraryA(dll_path);
    if (!dll) {
        loader_log("LoadLibrary(%s) failed: %lu", dll_path, GetLastError());
        return 0;
    }

    /* Call overlay_init to wire the runtime callbacks. */
    typedef void (*InitFn)(const OverlayCallbacks *);
    InitFn init_fn = (InitFn)GetProcAddress(dll, "overlay_init");
    if (!init_fn) {
        loader_log("no overlay_init in %s", dll_path);
        FreeLibrary(dll);
        return 0;
    }
    init_fn(&s_callbacks);

    /* Enumerate PE export table for func_XXXXXXXX symbols. */
    BYTE  *base   = (BYTE *)dll;
    IMAGE_DOS_HEADER    *dos  = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS    *nt   = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *exp_dd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

    if (!exp_dd->VirtualAddress) {
        loader_log("no export dir in %s", dll_path);
        FreeLibrary(dll);
        return 0;
    }

    IMAGE_EXPORT_DIRECTORY *exp =
        (IMAGE_EXPORT_DIRECTORY *)(base + exp_dd->VirtualAddress);
    DWORD *names   = (DWORD *)(base + exp->AddressOfNames);
    WORD  *ordinals = (WORD  *)(base + exp->AddressOfNameOrdinals);
    DWORD *funcs   = (DWORD *)(base + exp->AddressOfFunctions);

    int registered = 0;
    uint32_t fn_lo = 0xFFFFFFFFu, fn_hi = 0;
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char *name = (const char *)(base + names[i]);
        /* Match "func_XXXXXXXX" — exactly 13 chars */
        if (strncmp(name, "func_", 5) != 0) continue;
        if (strlen(name) != 13) continue;
        uint32_t addr = (uint32_t)strtoul(name + 5, NULL, 16);
        if (addr == 0) continue;

        WORD ord = ordinals[i];
        OverlayFn fn = (OverlayFn)(base + funcs[ord]);

        uint32_t phys = addr & 0x1FFFFFFFu;
        dyntab_insert(phys, fn, region);
        if (phys < fn_lo) fn_lo = phys;
        if (phys > fn_hi) fn_hi = phys;
        registered++;
    }
    /* Record the code span (function-entry extent) for watch + hash. We watch
     * through the page of the last entry only — NOT a page beyond, which would
     * re-include the overlay's data and cause false invalidations. A reload
     * rewrites the entries themselves, so invalidation still fires from those. */
    if (registered > 0 && region >= 0) {
        s_regions[region].fn_lo = fn_lo;
        s_regions[region].fn_hi = fn_hi + 4u;
    }

    loader_log("loaded %s -> %d functions registered", dll_path, registered);
    return registered;
}
#else
static int load_overlay_dll(const char *dll_path, uint32_t load_addr_virt, int region)
{
    (void)region;
    void *dll = dlopen(dll_path, RTLD_NOW | RTLD_LOCAL);
    if (!dll) {
        loader_log("dlopen(%s) failed: %s", dll_path, dlerror());
        return 0;
    }

    typedef void (*InitFn)(const OverlayCallbacks *);
    InitFn init_fn = (InitFn)dlsym(dll, "overlay_init");
    if (!init_fn) {
        loader_log("no overlay_init in %s", dll_path);
        dlclose(dll);
        return 0;
    }
    init_fn(&s_callbacks);

    /* On non-Windows we can't easily enumerate exports without libelf/BFD.
     * Fall back to scanning the known address range: probe GetProcAddress
     * equivalents by constructing names from the seed list stored in the
     * overlay capture.  For now, register nothing — the interpreter handles
     * it until proper export enumeration is implemented. */
    loader_log("%s loaded (posix export scan TODO)", dll_path);
    return 0;
}
#endif

/* ---- Public API -------------------------------------------------------- */

void overlay_loader_init(const char *cache_dir, const char *game_id)
{
    strncpy(s_cache_dir, cache_dir, sizeof(s_cache_dir) - 1);
    strncpy(s_game_id,   game_id,   sizeof(s_game_id)   - 1);
    init_callbacks();
    scan_cache_dir();
    s_active = 1;
}

void overlay_loader_check_cache(uint32_t load_addr, uint32_t size,
                                const uint8_t *bytes)
{
    /* Block-level check is not used — the DLL CRC is over the full assembled
     * region, not individual DMA blocks.  Cache loading is deferred to
     * overlay_loader_dispatch on the first dispatch miss. */
    (void)load_addr; (void)size; (void)bytes;
}

/* ---- Lazy region cache check ------------------------------------------- */
/* On the first dispatch miss for any address in a dirty region, we compute
 * the CRC32 of the full assembled region from RAM and look for a matching
 * DLL.  This fires AFTER the game has fully loaded the overlay and called
 * into it — by that point all DMA blocks have landed in RAM. */

#define MAX_CHECKED 64
static uint32_t s_checked[MAX_CHECKED];
static int      s_nchecked = 0;
static uint32_t s_last_crc = 0;
static int      s_last_file_found = 0;

static int already_checked(uint32_t region_start) {
    int i;
    for (i = 0; i < s_nchecked; i++)
        if (s_checked[i] == region_start) return 1;
    return 0;
}

static void mark_checked(uint32_t region_start) {
    if (s_nchecked < MAX_CHECKED)
        s_checked[s_nchecked++] = region_start;
}

static void try_load_region(uint32_t phys)
{
    extern uint32_t dirty_ram_get_bitmap_word(uint32_t word_index);
    extern uint32_t dirty_ram_get_bitmap_word_count(void);
    extern uint8_t *memory_get_ram_ptr(void);

    uint32_t page_sz = 4096u;
    uint32_t bw      = dirty_ram_get_bitmap_word_count();
    char dll_path[768];

    /* Walk backward to find region start. */
    uint32_t pg = phys / page_sz;
    while (pg > 0) {
        uint32_t pp = pg - 1;
        if (!((dirty_ram_get_bitmap_word(pp >> 5) >> (pp & 31u)) & 1u)) break;
        pg = pp;
    }
    uint32_t region_start = pg * page_sz;

    if (already_checked(region_start)) return;
    mark_checked(region_start);

    /* Walk forward to find region end. */
    uint32_t pg2 = phys / page_sz + 1;
    while (pg2 < bw * 32u &&
           ((dirty_ram_get_bitmap_word(pg2 >> 5) >> (pg2 & 31u)) & 1u))
        pg2++;
    uint32_t region_size = (pg2 - pg) * page_sz;

    /* Look up the compiled DLL by region_start in the cache index.
     * The index was built at init by scanning for {addr}_{crc}.dll files,
     * so we never need to recompute the CRC from RAM or DMA blocks. */
    s_last_crc = 0;
    s_last_file_found = 0;
    (void)region_size;

    int ci;
    const char *found_path = NULL;
    for (ci = 0; ci < s_cache_idx_count; ci++) {
        if (s_cache_idx[ci].region_start == region_start) {
            found_path = s_cache_idx[ci].path;
            break;
        }
    }
    if (!found_path) return;

    strncpy(dll_path, found_path, sizeof(dll_path) - 1);
    s_last_file_found = 1;

    if (s_nregions >= MAX_OVL_REGIONS) return;
    int region = s_nregions;
    s_regions[region].base        = region_start;
    s_regions[region].size        = region_size;
    s_regions[region].generation  = 0;
    s_regions[region].valid       = 1;
    s_regions[region].func_count  = 0;

    int registered = load_overlay_dll(
        dll_path, 0x80000000u | (region_start & 0x1FFFFFFFu), region);
    if (registered <= 0) {
        /* Nothing registered — don't keep an empty/watched region. The
         * already-checked mark stays so we don't retry the same miss. */
        return;
    }
    s_regions[region].func_count = registered;
    /* Use the code span (function-entry extent), not the whole dirty region,
     * so the overlay's own data writes don't churn the registration. fn_lo/fn_hi
     * were set by load_overlay_dll. */
    uint32_t code_lo  = s_regions[region].fn_lo;
    uint32_t code_len = s_regions[region].fn_hi - code_lo;
    /* Content hash of the live code span — recognizes the same overlay when it
     * loads back later (Inc2 reload-on-return). Code is identical across loads;
     * data (which we excluded) is not, so this is the reliable identity. */
    {
        extern uint8_t *memory_get_ram_ptr(void);
        uint8_t *ram = memory_get_ram_ptr();
        s_regions[region].crc_live = crc32_compute(ram + code_lo, code_len);
    }
    s_regions[region].writes_since_invalid = 0;
    s_nregions++;
    s_loads++;

    /* Watch the code span's RAM pages: a write there invalidates the
     * registration (Inc1-D). overlay_watch_set_range lives in memory.c, on
     * the single psx_write_* store chokepoint. */
    extern void overlay_watch_set_range(uint32_t phys, uint32_t len);
    overlay_watch_set_range(code_lo, code_len);
}

/* Invalidate the registered region containing `phys` (called from the store
 * path in memory.c when a write lands on a watched overlay page).  Coarse:
 * the whole region's registrations become non-callable. The DLL stays loaded;
 * only callability (region validity) is revoked. The already-checked mark is
 * left in place so the stale DLL is not auto-reloaded for changed bytes — the
 * new content falls back to the interpreter (re-registration of a matching
 * cache is a later increment). */
void overlay_loader_invalidate_at(uint32_t phys, uint32_t size)
{
    extern uint32_t g_debug_last_store_pc;
    uint32_t p = phys & 0x1FFFFFFFu;
    int r;
    for (r = 0; r < s_nregions; r++) {
        if (p < s_regions[r].base) continue;
        if (p >= s_regions[r].base + s_regions[r].size) continue;

        if (s_regions[r].valid) {
            /* First write into a live region: revoke callability. Keep
             * watching so we can detect a later reload of the same overlay. */
            s_regions[r].valid = 0;
            s_regions[r].generation++;
            s_regions[r].writes_since_invalid = 0;
            s_invalidations++;
            s_unregistered += (uint32_t)s_regions[r].func_count;
            s_last_write_pc   = g_debug_last_store_pc;
            s_last_write_addr = phys;
            s_last_write_size = size;
            loader_log("invalidated region 0x%08X (+%u) on write 0x%08X -> %d funcs",
                       s_regions[r].base, s_regions[r].size, phys,
                       s_regions[r].func_count);
        } else {
            /* Already invalid: count writes so a substantial rewrite (a real
             * reload, not incidental data writes) can trigger one re-hash. */
            s_regions[r].writes_since_invalid += size;
        }
        return;
    }
}

/* Attempt to re-validate an invalidated region whose RAM was substantially
 * rewritten (i.e. the overlay likely loaded back). One content hash; on a
 * match to the originally-registered hash, the 88 functions become callable
 * again with no DLL reload (the dyntab entries never left). Cheap-gated: only
 * hashes after a near-full rewrite, then resets the counter. */
static void try_revalidate(uint32_t phys)
{
    extern uint8_t *memory_get_ram_ptr(void);
    uint32_t p = phys & 0x1FFFFFFFu;
    int r;
    for (r = 0; r < s_nregions; r++) {
        if (s_regions[r].valid) continue;
        if (p < s_regions[r].base) continue;
        if (p >= s_regions[r].base + s_regions[r].size) continue;
        uint32_t code_lo  = s_regions[r].fn_lo;
        uint32_t code_len = s_regions[r].fn_hi - code_lo;
        /* Require a near-full rewrite of the code span before paying for a hash
         * (a real reload, not incidental writes). */
        if (s_regions[r].writes_since_invalid < (code_len / 2u)) return;
        s_regions[r].writes_since_invalid = 0;

        uint8_t *ram = memory_get_ram_ptr();
        uint32_t crc = crc32_compute(ram + code_lo, code_len);
        s_reval_attempts++;
        s_last_reval_crc = crc;
        if (crc == s_regions[r].crc_live) {
            s_regions[r].valid = 1;
            s_regions[r].generation++;
            s_revalidations++;
            loader_log("re-validated region 0x%08X (+%u) on reload -> %d funcs native",
                       s_regions[r].base, s_regions[r].size, s_regions[r].func_count);
        } else {
            s_reval_crc_miss++;
            loader_log("reval miss region 0x%08X span[%08X,%08X) crc=%08X want=%08X",
                       s_regions[r].base, code_lo, s_regions[r].fn_hi,
                       crc, s_regions[r].crc_live);
        }
        return;
    }
}

int overlay_loader_dispatch(CPUState *cpu, uint32_t addr)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    OverlayFn fn  = dyntab_lookup(phys);
    if (fn) { s_disp_native++; fn(cpu); return 1; }

    if (s_active && phys >= 0x98000u) {
        /* Same overlay loaded back into an invalidated region? Re-enable its
         * functions natively (Inc2). Cheap: only hashes after a near-full
         * rewrite. */
        try_revalidate(phys);
        fn = dyntab_lookup(phys);
        if (fn) { s_disp_native++; fn(cpu); return 1; }
        /* First miss for a genuinely new region: load its cached DLL. */
        try_load_region(phys);
    }

    /* Retry after potential DLL load. */
    fn = dyntab_lookup(phys);
    if (fn) { s_disp_native++; fn(cpu); return 1; }

    s_disp_interp++;
    return 0;
}

void overlay_loader_get_counters(uint32_t *loads, uint32_t *invalidations,
                                 uint32_t *unregistered,
                                 uint64_t *disp_native, uint64_t *disp_interp,
                                 uint64_t *stale_blocked,
                                 uint32_t *last_write_pc,
                                 uint32_t *last_write_addr,
                                 uint32_t *last_write_size,
                                 int *regions, uint32_t *revalidations)
{
    if (loads)           *loads           = s_loads;
    if (invalidations)   *invalidations   = s_invalidations;
    if (unregistered)    *unregistered    = s_unregistered;
    if (disp_native)     *disp_native     = s_disp_native;
    if (disp_interp)     *disp_interp     = s_disp_interp;
    if (stale_blocked)   *stale_blocked   = s_stale_blocked;
    if (last_write_pc)   *last_write_pc   = s_last_write_pc;
    if (last_write_addr) *last_write_addr = s_last_write_addr;
    if (last_write_size) *last_write_size = s_last_write_size;
    if (regions)         *regions         = s_nregions;
    if (revalidations)   *revalidations   = s_revalidations;
}

/* Inc2 reload diagnostics — region 0 state + revalidate attempt/miss counters. */
void overlay_loader_get_reload_debug(int *r0_valid, uint32_t *r0_writes,
                                     uint32_t *r0_fn_lo, uint32_t *r0_fn_hi,
                                     uint32_t *r0_crc_live,
                                     uint32_t *reval_attempts,
                                     uint32_t *reval_crc_miss,
                                     uint32_t *last_reval_crc)
{
    if (s_nregions > 0) {
        if (r0_valid)    *r0_valid    = s_regions[0].valid;
        if (r0_writes)   *r0_writes   = s_regions[0].writes_since_invalid;
        if (r0_fn_lo)    *r0_fn_lo    = s_regions[0].fn_lo;
        if (r0_fn_hi)    *r0_fn_hi    = s_regions[0].fn_hi;
        if (r0_crc_live) *r0_crc_live = s_regions[0].crc_live;
    }
    if (reval_attempts) *reval_attempts = s_reval_attempts;
    if (reval_crc_miss) *reval_crc_miss = s_reval_crc_miss;
    if (last_reval_crc) *last_reval_crc = s_last_reval_crc;
}

int overlay_loader_registered_count(void)
{
    return s_count;
}

void overlay_loader_get_status(int *active, int *registered,
                               int *regions_checked,
                               char *cache_dir_out, int cache_dir_len,
                               char *game_id_out,   int game_id_len,
                               uint32_t *checked_out, int checked_max,
                               int *checked_written,
                               uint32_t *last_crc_out, int *last_file_found_out)
{
    if (active)          *active          = s_active;
    if (registered)      *registered      = s_count;
    if (regions_checked) *regions_checked = s_nchecked;
    if (cache_dir_out)   strncpy(cache_dir_out, s_cache_dir, (size_t)cache_dir_len - 1);
    if (game_id_out)     strncpy(game_id_out,   s_game_id,   (size_t)game_id_len   - 1);
    if (checked_out && checked_written) {
        int n = s_nchecked < checked_max ? s_nchecked : checked_max;
        for (int i = 0; i < n; i++) checked_out[i] = s_checked[i];
        *checked_written = n;
    }
    if (last_crc_out)       *last_crc_out       = s_last_crc;
    if (last_file_found_out) *last_file_found_out = s_last_file_found;
}

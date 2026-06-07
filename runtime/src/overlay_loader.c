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
} DynEntry;

static DynEntry  s_table[DYNTAB_CAP];
static int       s_count = 0;

static void dyntab_insert(uint32_t phys, OverlayFn fn)
{
    uint32_t h = (phys * 2654435761u) & DYNTAB_MASK;
    uint32_t i;
    for (i = 0; i < DYNTAB_CAP; i++) {
        uint32_t idx = (h + i) & DYNTAB_MASK;
        if (s_table[idx].addr == 0 || s_table[idx].addr == phys) {
            if (s_table[idx].addr == 0) s_count++;
            s_table[idx].addr = phys;
            s_table[idx].fn   = fn;
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
        if (s_table[idx].addr == phys) return s_table[idx].fn;
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
static int load_overlay_dll(const char *dll_path, uint32_t load_addr_virt)
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
        dyntab_insert(phys, fn);
        registered++;
    }

    loader_log("loaded %s -> %d functions registered", dll_path, registered);
    return registered;
}
#else
static int load_overlay_dll(const char *dll_path, uint32_t load_addr_virt)
{
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
    load_overlay_dll(dll_path, 0x80000000u | (region_start & 0x1FFFFFFFu));
}

int overlay_loader_dispatch(CPUState *cpu, uint32_t addr)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    OverlayFn fn  = dyntab_lookup(phys);
    if (fn) { fn(cpu); return 1; }

    /* First miss for this region: check if a DLL is cached. */
    if (s_active && phys >= 0x98000u)
        try_load_region(phys);

    /* Retry after potential DLL load. */
    fn = dyntab_lookup(phys);
    if (fn) { fn(cpu); return 1; }

    return 0;
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

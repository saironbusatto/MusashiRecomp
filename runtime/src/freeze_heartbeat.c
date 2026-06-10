/* freeze_heartbeat.c — see header for rationale. */

#include "freeze_heartbeat.h"
#include "debug_server.h"
#include "crash_trace.h"   /* g_psx_fatal_reason */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

/* State accessors. All defined in other compilation units; declared here
 * to avoid pulling in heavy headers. */
extern uint64_t s_frame_count;                 /* debug_server.c */
extern uint32_t g_debug_current_func_addr;     /* debug_server.c */
extern uint32_t g_debug_last_store_pc;         /* debug_server.c */
extern uint32_t i_stat;                        /* memory.c */
extern uint32_t i_mask;                        /* memory.c */

extern uint64_t psx_get_cycle_count(void);
extern void     psx_get_freeze_diag(uint64_t *total_checks,
                                    uint32_t *dispatch_count,
                                    int *in_exception,
                                    int *post_exception_cooldown,
                                    uint64_t *exception_entries,
                                    uint64_t *exception_reentry_blocks);
extern void     sio_get_freeze_diag(int *irq_pending, int *irq_countdown,
                                    uint16_t *sio_stat_out, uint16_t *sio_ctrl_out,
                                    int *card_active);
extern int      sio_get_mc_max_state(void);
extern int      sio_get_tx_writes(void);

/* dirty_ram_interp counters — defined as plain globals in dirty_ram_interp.c.
 * Declared weakly here in case a future build drops the unit; missing symbol
 * would only fail at link time and is easy to catch. */
extern uint64_t g_dirty_ram_blocks_run;
extern uint64_t g_dirty_ram_insns_run;

/* Vsync self-heal counters — defined in main.cpp. Included in the dump
 * so a slow_frames wedge can be attributed to driver present
 * backpressure straight from the JSON. */
extern uint32_t g_present_slow_count;
extern int      g_present_vsync_disabled;

static int s_started = 0;
static char s_backend[32] = "psx-runtime";

#ifdef _WIN32
static HANDLE s_thread = NULL;
static HANDLE s_main_thread = NULL;       /* DuplicateHandle of main thread */
static DWORD  s_main_thread_id = 0;
static int    s_sym_initialized = 0;
#endif

#define HB_FILE        "psx_freeze_heartbeat.json"
#define HB_INTERVAL_MS 100u

/* Wedge detection.
 *
 * Three failure modes to catch:
 *   A. HARD freeze — main thread is fully wedged; frame_count stops
 *      advancing entirely. Detected by zero frame_delta over the window.
 *      (Bug B root cause was this kind: the old pacing loop's double
 *      counter read underflowed into a ~24.7-day SDL_Delay.)
 *   B. REENTRY storm — frames advance but exception-handler reentry
 *      per frame is far above the chronic baseline. The baseline is
 *      ~2K reentry blocks per frame (the whole VSync callback chain
 *      runs in exception context, every block leader checks), so the
 *      storm test must be normalized PER FRAME, not per second. The
 *      old absolute threshold (10K per 2s window = 5K/sec) was below
 *      the healthy baseline (~2K/frame x 30-60 fps = 60-120K/sec) and
 *      labeled normal play, boots, and host-side slowdowns all as
 *      "reentry_storm" — which twice sent freeze investigations down
 *      the wrong path.
 *   C. SLOW frames — frames advance but pathologically slowly while
 *      per-frame PSX work is normal: the wall clock is being eaten
 *      outside the simulation (observed: NVIDIA GL SwapBuffers
 *      blocking ~1.5s per present, dump 1781045865). Multi-sample
 *      stack capture attributes the wait.
 *
 * Window = 20 ticks = 2.0 sec. Long enough that legitimate startup
 * activity (boot, FMV decode, save load) doesn't trip it; short enough
 * that real wedges dump within seconds. */
#define WEDGE_WINDOW_TICKS 20u
#define WEDGE_EXC_REENTRY_PER_FRAME_THRESHOLD 20000u /* ~10x chronic 2K/frame */
#define WEDGE_SLOW_FRAMES_MAX_DELTA 10u   /* <5 fps avg over the 2s window */

/* Per-ring caps for auto-dump. Newest-first window. Sized so the total
 * dump fits in ~10-15 MB JSON even when every ring is full. */
#define DUMP_CAP_WTRACE_ALL    16384u
#define DUMP_CAP_WTRACE         8192u
#define DUMP_CAP_FRAME_HISTORY   600u
#define DUMP_CAP_SIO_PC         8192u
#define DUMP_CAP_THREAD_TRACE   4096u
#define DUMP_CAP_RESTORE_TRACE  4096u
#define DUMP_CAP_FN_ENTRY       4096u
#define DUMP_CAP_DIRTY_BLOCK   16384u

/* Pre-freeze history ring. Each entry = a snapshot taken at one heartbeat
 * tick (~100 ms). When the runtime freezes, all the "now" values stop
 * advancing but the past N entries still show what state it was in for
 * the seconds leading up to the stall. RING_CAP * 100ms = window length. */
#define RING_CAP 64
typedef struct {
    uint64_t frame_count;
    uint64_t psx_cycle_count;
    uint64_t exc_reentry;
    uint64_t dirty_ram_insns;
    uint32_t current_func;
    uint32_t last_store_pc;
    uint32_t i_stat;
    uint16_t sio_stat;
    uint16_t sio_ctrl;
    uint8_t  in_exception;
    uint8_t  mc_max_state;
    long long wall_clock;
} HbRingEntry;
static HbRingEntry s_ring[RING_CAP];
static uint32_t    s_ring_head = 0;
static uint32_t    s_ring_count = 0;

/* Wedge detection state.
 *   s_dump_armed - 1 if a wedge dump is allowed to fire; cleared after
 *                  firing, re-armed when the wedge clears (healthy tick). */
static int      s_dump_armed = 1;
static uint32_t s_last_wedge_kind = 0;  /* informational, last detected kind */

#ifdef _WIN32
/* Capture the main thread's call stack at the moment of a hard freeze.
 *
 * Called from the heartbeat thread when wedge_kind==1 (frame_count not
 * advancing). Suspends the main thread, walks its stack via StackWalk64,
 * symbolizes via SymFromAddr, then resumes. Each frame becomes a JSON
 * object {addr, symbol?, displacement?, module?}.
 *
 * Best-effort: if DbgHelp init fails, dump an empty array and continue. */
static void freeze_dump_main_stack_json(FILE *f) {
    if (!f) return;
    if (!s_main_thread) { fputs("[]", f); return; }

    if (!s_sym_initialized) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (SymInitialize(GetCurrentProcess(), NULL, TRUE)) {
            s_sym_initialized = 1;
        } else {
            fputs("[]", f);
            return;
        }
    }

    DWORD susp_count = SuspendThread(s_main_thread);
    if (susp_count == (DWORD)-1) { fputs("[]", f); return; }

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(s_main_thread, &ctx)) {
        ResumeThread(s_main_thread);
        fputs("[]", f);
        return;
    }

    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#else
    DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrStack.Offset = ctx.Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    fputc('[', f);
    int first = 1;
    union {
        SYMBOL_INFO si;
        char buf[sizeof(SYMBOL_INFO) + 512];
    } sym_storage;
    SYMBOL_INFO *sym = &sym_storage.si;

    for (int depth = 0; depth < 128; depth++) {
        if (!StackWalk64(machine, GetCurrentProcess(), s_main_thread, &frame,
                         &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64,
                         NULL)) {
            break;
        }
        DWORD64 addr = frame.AddrPC.Offset;
        if (!addr) break;

        memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 511;
        DWORD64 displacement = 0;
        BOOL got_sym = SymFromAddr(GetCurrentProcess(), addr, &displacement, sym);

        IMAGEHLP_MODULE64 mod;
        memset(&mod, 0, sizeof(mod));
        mod.SizeOfStruct = sizeof(mod);
        BOOL got_mod = SymGetModuleInfo64(GetCurrentProcess(), addr, &mod);

        fprintf(f, "%s{\"depth\":%d,\"addr\":\"0x%016llX\"",
                first ? "" : ",", depth, (unsigned long long)addr);
        if (got_sym) {
            /* Escape JSON-special chars in symbol name (best-effort: bracket replace). */
            char safe[512];
            size_t k = 0;
            for (size_t i = 0; sym->Name[i] && k < sizeof(safe) - 2; i++) {
                char c = sym->Name[i];
                if (c == '"' || c == '\\') safe[k++] = '_';
                else if ((unsigned char)c < 0x20) safe[k++] = '_';
                else safe[k++] = c;
            }
            safe[k] = 0;
            fprintf(f, ",\"symbol\":\"%s\",\"displacement\":%llu",
                    safe, (unsigned long long)displacement);
        }
        if (got_mod && mod.ModuleName[0]) {
            fprintf(f, ",\"module\":\"%s\"", mod.ModuleName);
        }
        fputc('}', f);
        first = 0;
    }
    fputc(']', f);

    ResumeThread(s_main_thread);
}

/* Multi-sample stack capture for still-running wedges (wedge_kind 2 and 3).
 * The main thread is still running (bouncing through the exception/VSync
 * dispatch), so a single snapshot could land on a random recompiled block.
 * Taking N snapshots a few ms apart reveals the RECURRING frames (the
 * persistent dispatch/handler chain), which is what we want. Emits a JSON
 * array of per-sample frame arrays: [[...],[...],...]. */
static void freeze_dump_main_stack_samples_json(FILE *f, int n) {
    if (!f) return;
    fputc('[', f);
    for (int i = 0; i < n; i++) {
        if (i) fputc(',', f);
        freeze_dump_main_stack_json(f);   /* one suspend/walk/resume snapshot */
        Sleep(2);
    }
    fputc(']', f);
}
#endif /* _WIN32 */

static void freeze_dump_write(long long wall, uint64_t frame, uint64_t cyc,
                              uint64_t exc_reentry, uint32_t cur_fn,
                              uint32_t last_store, uint32_t i_stat_v,
                              uint32_t i_mask_v, int in_exc,
                              uint64_t total_checks, uint32_t dispatch_count,
                              uint64_t exc_entries,
                              uint16_t sio_stat_v, uint16_t sio_ctrl_v,
                              int sio_card_active, int mc_max, int tx_writes)
{
    char path[128];
    snprintf(path, sizeof(path),
             "psx_freeze_dump_%s_%lld.json", s_backend, wall);

    FILE *f = fopen(path, "wb");
    if (!f) return;

    /* Large stdio buffer so multi-MB JSON arrays write efficiently. */
    static char io_buf[1 << 16];
    setvbuf(f, io_buf, _IOFBF, sizeof(io_buf));

    fprintf(f,
        "{\n"
        "  \"backend\":\"%s\",\n"
        "  \"wall_clock_epoch\":%lld,\n"
        "  \"frame_count\":%llu,\n"
        "  \"psx_cycle_count\":%llu,\n"
        "  \"current_func\":\"0x%08X\",\n"
        "  \"last_store_pc\":\"0x%08X\",\n"
        "  \"total_checks\":%llu,\n"
        "  \"dispatch_count\":%u,\n"
        "  \"in_exception\":%d,\n"
        "  \"exception_entries\":%llu,\n"
        "  \"exception_reentry_blocks\":%llu,\n"
        "  \"sio_stat\":\"0x%04X\",\n"
        "  \"sio_ctrl\":\"0x%04X\",\n"
        "  \"sio_card_active\":%d,\n"
        "  \"i_stat\":\"0x%08X\",\n"
        "  \"i_mask\":\"0x%08X\",\n"
        "  \"mc_max_state\":%d,\n"
        "  \"tx_writes\":%d,\n"
        "  \"dirty_ram_blocks\":%llu,\n"
        "  \"dirty_ram_insns\":%llu,\n"
        "  \"present_slow_count\":%u,\n"
        "  \"present_vsync_disabled\":%d,\n"
        "  \"wedge_kind\":%u,\n"
        "  \"wedge_kind_name\":\"%s\",\n"
        "  \"caps\":{\"wtrace_all\":%u,\"wtrace\":%u,\"frames\":%u,"
                  "\"sio_pc\":%u,\"thread\":%u,\"restore\":%u,\"fn_entry\":%u,"
                  "\"dirty_block\":%u},\n",
        s_backend, wall,
        (unsigned long long)frame, (unsigned long long)cyc,
        cur_fn, last_store,
        (unsigned long long)total_checks, dispatch_count,
        in_exc,
        (unsigned long long)exc_entries,
        (unsigned long long)exc_reentry,
        (unsigned)sio_stat_v, (unsigned)sio_ctrl_v,
        sio_card_active,
        i_stat_v, i_mask_v,
        mc_max, tx_writes,
        (unsigned long long)g_dirty_ram_blocks_run,
        (unsigned long long)g_dirty_ram_insns_run,
        g_present_slow_count,
        g_present_vsync_disabled,
        s_last_wedge_kind,
        (s_last_wedge_kind == 1) ? "hard_freeze" :
        (s_last_wedge_kind == 2) ? "reentry_storm" :
        (s_last_wedge_kind == 3) ? "slow_frames" :
        (s_last_wedge_kind == 4) ? "fatal" : "unknown",
        (unsigned)DUMP_CAP_WTRACE_ALL,
        (unsigned)DUMP_CAP_WTRACE,
        (unsigned)DUMP_CAP_FRAME_HISTORY,
        (unsigned)DUMP_CAP_SIO_PC,
        (unsigned)DUMP_CAP_THREAD_TRACE,
        (unsigned)DUMP_CAP_RESTORE_TRACE,
        (unsigned)DUMP_CAP_FN_ENTRY,
        (unsigned)DUMP_CAP_DIRTY_BLOCK);

    fputs("  \"heartbeat_ring\":[\n", f);
    uint32_t avail = s_ring_count;
    uint32_t start = (s_ring_count < RING_CAP) ? 0 : s_ring_head;
    for (uint32_t i = 0; i < avail; i++) {
        HbRingEntry *e = &s_ring[(start + i) % RING_CAP];
        fprintf(f,
            "    {\"wall\":%lld,\"frame\":%llu,\"cyc\":%llu,"
            "\"exc_re\":%llu,\"dirty_insns\":%llu,"
            "\"cur_fn\":\"0x%08X\",\"store_pc\":\"0x%08X\","
            "\"i_stat\":\"0x%08X\",\"sio_stat\":\"0x%04X\","
            "\"sio_ctrl\":\"0x%04X\",\"in_exc\":%u,\"mc_max\":%u}%s\n",
            e->wall_clock,
            (unsigned long long)e->frame_count,
            (unsigned long long)e->psx_cycle_count,
            (unsigned long long)e->exc_reentry,
            (unsigned long long)e->dirty_ram_insns,
            e->current_func, e->last_store_pc,
            e->i_stat, (unsigned)e->sio_stat, (unsigned)e->sio_ctrl,
            (unsigned)e->in_exception, (unsigned)e->mc_max_state,
            (i + 1 < avail) ? "," : "");
    }
    fputs("  ],\n", f);

    fputs("  \"wtrace_all\":", f);
    debug_server_freeze_dump_wtrace_all_json(f, DUMP_CAP_WTRACE_ALL);
    fputs(",\n", f);

    fputs("  \"wtrace\":", f);
    debug_server_freeze_dump_wtrace_json(f, DUMP_CAP_WTRACE);
    fputs(",\n", f);

    fputs("  \"frame_history\":", f);
    debug_server_freeze_dump_frame_history_json(f, DUMP_CAP_FRAME_HISTORY);
    fputs(",\n", f);

    fputs("  \"sio_pc_trace\":", f);
    debug_server_freeze_dump_sio_pc_json(f, DUMP_CAP_SIO_PC);
    fputs(",\n", f);

    fputs("  \"thread_trace\":", f);
    debug_server_freeze_dump_thread_trace_json(f, DUMP_CAP_THREAD_TRACE);
    fputs(",\n", f);

    fputs("  \"restore_trace\":", f);
    debug_server_freeze_dump_restore_trace_json(f, DUMP_CAP_RESTORE_TRACE);
    fputs(",\n", f);

    fputs("  \"fn_entry\":", f);
    debug_server_freeze_dump_fn_entry_json(f, DUMP_CAP_FN_ENTRY);
    fputs(",\n", f);

    fputs("  \"dirty_block\":", f);
    debug_server_freeze_dump_dirty_block_json(f, DUMP_CAP_DIRTY_BLOCK);
    fputs(",\n", f);

#ifdef _WIN32
    /* Main-thread call stack. Hard freeze (wedge_kind==1): one snapshot of the
     * wedged thread in `main_stack`. Still-running wedges (kind 2 reentry
     * storm, kind 3 slow frames): a single frame is noise — take multiple
     * samples in `main_stack_samples` to expose the recurring wait (the
     * dispatch/handler chain, or a driver present block). */
    fputs("  \"main_stack\":", f);
    if (s_last_wedge_kind == 1) {
        freeze_dump_main_stack_json(f);
    } else {
        fputs("[]", f);
    }
    fputs(",\n  \"main_stack_samples\":", f);
    if (s_last_wedge_kind == 2 || s_last_wedge_kind == 3) {
        freeze_dump_main_stack_samples_json(f, 8);
    } else {
        fputs("[]", f);
    }
    fputs("\n", f);
#else
    fputs("  \"main_stack\":[]\n", f);
#endif

    fputs("}\n", f);
    fclose(f);
}

/* Full ring dump for deliberate fatal sites (psx_fatal_halt). Runs ON the
 * faulting (main) thread, unlike the watchdog path — wedge kind 4 keeps
 * freeze_dump_write away from the SuspendThread-based stack walkers,
 * which would self-suspend here. Gathers the same diag snapshot the
 * heartbeat tick does so the dump is self-contained. */
void freeze_heartbeat_fatal_dump(const char *reason) {
    (void)reason;  /* reason travels via g_psx_fatal_reason in the heartbeat */

    uint64_t total_checks = 0;
    uint32_t dispatch_count = 0;
    int in_exc = 0;
    int post_cool = 0;
    uint64_t exc_entries = 0;
    uint64_t exc_reentry = 0;
    psx_get_freeze_diag(&total_checks, &dispatch_count, &in_exc,
                        &post_cool, &exc_entries, &exc_reentry);

    int sio_irq_pending = 0;
    int sio_irq_countdown = 0;
    uint16_t sio_stat = 0;
    uint16_t sio_ctrl = 0;
    int card_active = 0;
    sio_get_freeze_diag(&sio_irq_pending, &sio_irq_countdown,
                        &sio_stat, &sio_ctrl, &card_active);

    s_last_wedge_kind = 4;
    s_dump_armed = 0;  /* the watchdog must not overwrite the fatal dump */
    freeze_dump_write((long long)time(NULL), s_frame_count,
                      psx_get_cycle_count(), exc_reentry,
                      g_debug_current_func_addr, g_debug_last_store_pc,
                      i_stat, i_mask, in_exc, total_checks,
                      dispatch_count, exc_entries,
                      sio_stat, sio_ctrl, card_active,
                      sio_get_mc_max_state(), sio_get_tx_writes());
}

static void heartbeat_write(void) {
    uint64_t cyc = psx_get_cycle_count();
    uint64_t frame = s_frame_count;
    uint32_t cur_fn = g_debug_current_func_addr;
    uint32_t last_store = g_debug_last_store_pc;

    uint64_t total_checks = 0;
    uint32_t dispatch_count = 0;
    int in_exc = 0;
    int post_cool = 0;
    uint64_t exc_entries = 0;
    uint64_t exc_reentry = 0;
    psx_get_freeze_diag(&total_checks, &dispatch_count, &in_exc,
                        &post_cool, &exc_entries, &exc_reentry);

    int sio_irq_pending = 0;
    int sio_irq_countdown = 0;
    uint16_t sio_stat = 0;
    uint16_t sio_ctrl = 0;
    int card_active = 0;
    sio_get_freeze_diag(&sio_irq_pending, &sio_irq_countdown,
                        &sio_stat, &sio_ctrl, &card_active);

    int mc_max = sio_get_mc_max_state();
    int tx_writes = sio_get_tx_writes();

    /* Wall-clock seconds since epoch — coarse but enough to spot stalls. */
    long long wall = (long long)time(NULL);

    /* Push current state into the ring. The ring captures the seconds
     * leading up to the freeze; when main thread stalls, "current" values
     * stop advancing but the ring's older entries still show the recent
     * trajectory. */
    HbRingEntry *re = &s_ring[s_ring_head];
    re->frame_count     = frame;
    re->psx_cycle_count = cyc;
    re->exc_reentry     = exc_reentry;
    re->dirty_ram_insns = g_dirty_ram_insns_run;
    re->current_func    = cur_fn;
    re->last_store_pc   = last_store;
    re->i_stat          = i_stat;
    re->sio_stat        = sio_stat;
    re->sio_ctrl        = sio_ctrl;
    re->in_exception    = (uint8_t)in_exc;
    re->mc_max_state    = (uint8_t)mc_max;
    re->wall_clock      = wall;
    s_ring_head = (s_ring_head + 1) % RING_CAP;
    if (s_ring_count < RING_CAP) s_ring_count++;

    /* ---- Wedge detection: arm-once auto-dump ----
     * Walk back WEDGE_WINDOW_TICKS in the heartbeat ring (just pushed
     * above) and compute deltas. Trigger if any of:
     *   A. frame_count delta == 0                  (hard freeze)
     *   B. exc_reentry delta PER FRAME > threshold (reentry storm)
     *   C. frame_count delta < slow-frames floor   (host-side stall)
     *
     * Only fires once the ring has enough history. When the wedge clears
     * (a healthy tick), re-arm. */
    uint32_t wedge_kind = 0;  /* 0=healthy 1=hard 2=reentry storm 3=slow frames */
    if (s_ring_count >= WEDGE_WINDOW_TICKS) {
        /* The just-pushed tick is at (s_ring_head - 1). The oldest in
         * our window is WEDGE_WINDOW_TICKS - 1 ticks before it. */
        uint32_t newest_idx = (s_ring_head + RING_CAP - 1u) % RING_CAP;
        uint32_t oldest_idx = (s_ring_head + RING_CAP - WEDGE_WINDOW_TICKS) % RING_CAP;
        uint64_t newest_frame = s_ring[newest_idx].frame_count;
        uint64_t oldest_frame = s_ring[oldest_idx].frame_count;
        uint64_t newest_excre = s_ring[newest_idx].exc_reentry;
        uint64_t oldest_excre = s_ring[oldest_idx].exc_reentry;
        uint64_t frame_delta = (newest_frame >= oldest_frame)
                               ? (newest_frame - oldest_frame) : 0;
        uint64_t excre_delta = (newest_excre >= oldest_excre)
                               ? (newest_excre - oldest_excre) : 0;

        if (frame_delta == 0)
            wedge_kind = 1;
        else if (excre_delta / frame_delta > WEDGE_EXC_REENTRY_PER_FRAME_THRESHOLD)
            wedge_kind = 2;
        else if (frame_delta < WEDGE_SLOW_FRAMES_MAX_DELTA)
            wedge_kind = 3;
    }

    if (wedge_kind != 0) {
        if (s_dump_armed) {
            s_dump_armed = 0;
            s_last_wedge_kind = wedge_kind;
            freeze_dump_write(wall, frame, cyc, exc_reentry, cur_fn, last_store,
                              i_stat, i_mask, in_exc, total_checks,
                              dispatch_count, exc_entries,
                              sio_stat, sio_ctrl, card_active,
                              mc_max, tx_writes);
        }
    } else {
        /* Healthy tick: re-arm for the next wedge. */
        s_dump_armed = 1;
    }

    /* Buffer sized for current-state JSON + ring (~256B per ring entry). */
    static char buf[64 * 1024];
    int n = snprintf(buf, sizeof(buf),
        "{\n"
        "  \"backend\":\"%s\",\n"
        "  \"wall_clock_epoch\":%lld,\n"
        "  \"frame_count\":%llu,\n"
        "  \"psx_cycle_count\":%llu,\n"
        "  \"current_func\":\"0x%08X\",\n"
        "  \"last_store_pc\":\"0x%08X\",\n"
        "  \"total_checks\":%llu,\n"
        "  \"dispatch_count\":%u,\n"
        "  \"in_exception\":%d,\n"
        "  \"post_exception_cooldown\":%d,\n"
        "  \"exception_entries\":%llu,\n"
        "  \"exception_reentry_blocks\":%llu,\n"
        "  \"sio_irq_pending\":%d,\n"
        "  \"sio_irq_countdown\":%d,\n"
        "  \"sio_stat\":\"0x%04X\",\n"
        "  \"sio_ctrl\":\"0x%04X\",\n"
        "  \"sio_card_active\":%d,\n"
        "  \"i_stat\":\"0x%08X\",\n"
        "  \"i_mask\":\"0x%08X\",\n"
        "  \"mc_max_state\":%d,\n"
        "  \"tx_writes\":%d,\n"
        "  \"dirty_ram_blocks\":%llu,\n"
        "  \"dirty_ram_insns\":%llu,\n"
        "  \"fatal\":%s%s%s\n"
        "}\n",
        s_backend,
        wall,
        (unsigned long long)frame,
        (unsigned long long)cyc,
        cur_fn,
        last_store,
        (unsigned long long)total_checks,
        dispatch_count,
        in_exc,
        post_cool,
        (unsigned long long)exc_entries,
        (unsigned long long)exc_reentry,
        sio_irq_pending,
        sio_irq_countdown,
        (unsigned)sio_stat,
        (unsigned)sio_ctrl,
        card_active,
        i_stat, i_mask,
        mc_max,
        tx_writes,
        (unsigned long long)g_dirty_ram_blocks_run,
        (unsigned long long)g_dirty_ram_insns_run,
        g_psx_fatal_reason ? "\"" : "",
        g_psx_fatal_reason ? g_psx_fatal_reason : "null",
        g_psx_fatal_reason ? "\"" : "");

    if (n <= 0 || n >= (int)sizeof(buf)) return;

    /* Append pre-freeze history ring. Format: array of compact rows,
     * oldest first, newest last. Reader can spot the moment all values
     * stop advancing — that's the stall point. */
    /* Strip the closing brace of the main object so we can append. */
    if (n > 0 && buf[n - 1] == '\n') n--;     /* drop trailing newline */
    if (n > 0 && buf[n - 1] == '}')  n--;     /* drop closing brace */
    if (n > 0 && buf[n - 1] == '\n') n--;     /* and any preceding newline */

    int m = snprintf(buf + n, sizeof(buf) - (size_t)n, ",\n  \"ring\":[\n");
    if (m > 0) n += m;

    uint32_t avail = s_ring_count;
    uint32_t start = (s_ring_count < RING_CAP) ? 0
                   : (s_ring_head /* head points to next-to-write = oldest */);
    for (uint32_t i = 0; i < avail; i++) {
        HbRingEntry *e = &s_ring[(start + i) % RING_CAP];
        m = snprintf(buf + n, sizeof(buf) - (size_t)n,
            "    {\"wall\":%lld,\"frame\":%llu,\"cyc\":%llu,"
            "\"exc_re\":%llu,\"dirty_insns\":%llu,"
            "\"cur_fn\":\"0x%08X\",\"store_pc\":\"0x%08X\","
            "\"i_stat\":\"0x%08X\",\"sio_stat\":\"0x%04X\","
            "\"sio_ctrl\":\"0x%04X\",\"in_exc\":%u,\"mc_max\":%u}%s\n",
            e->wall_clock,
            (unsigned long long)e->frame_count,
            (unsigned long long)e->psx_cycle_count,
            (unsigned long long)e->exc_reentry,
            (unsigned long long)e->dirty_ram_insns,
            e->current_func, e->last_store_pc,
            e->i_stat, (unsigned)e->sio_stat, (unsigned)e->sio_ctrl,
            (unsigned)e->in_exception, (unsigned)e->mc_max_state,
            (i + 1 < avail) ? "," : "");
        if (m <= 0 || (size_t)(n + m) >= sizeof(buf)) break;
        n += m;
    }
    m = snprintf(buf + n, sizeof(buf) - (size_t)n, "  ]\n}\n");
    if (m > 0) n += m;

    /* Atomic overwrite via .tmp + rename. Avoids a reader catching a
     * mid-write file and parsing partial JSON. Cheap on Windows
     * (MoveFileEx with REPLACE_EXISTING). */
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), HB_FILE ".tmp");

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return;
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);

#ifdef _WIN32
    MoveFileExA(tmp_path, HB_FILE,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    rename(tmp_path, HB_FILE);
#endif
}

#ifdef _WIN32
static DWORD WINAPI heartbeat_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        heartbeat_write();
        Sleep(HB_INTERVAL_MS);
    }
}
#endif

void freeze_heartbeat_start(const char *backend_label) {
    /* INTENTIONALLY not gated by PSX_NO_DEBUG_TOOLS. The heartbeat thread
     * is the only observability mechanism that survives a main-thread
     * stall (TCP server is on main thread; debug log functions are
     * called from the stalled code). Cost is ~1 KB/sec disk write and
     * one extra thread — small enough to keep in production builds for
     * crash forensics. */
    if (s_started) return;
    if (backend_label && backend_label[0]) {
        size_t n = strlen(backend_label);
        if (n >= sizeof(s_backend)) n = sizeof(s_backend) - 1;
        memcpy(s_backend, backend_label, n);
        s_backend[n] = 0;
    }
#ifdef _WIN32
    /* Duplicate the main thread's pseudo-handle into a real handle so the
     * heartbeat thread can SuspendThread/GetThreadContext for stack capture
     * on hard freezes. GetCurrentThread() returns a pseudo-handle that's
     * only valid in the calling thread, so the duplication is required. */
    s_main_thread_id = GetCurrentThreadId();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &s_main_thread,
                         THREAD_ALL_ACCESS, FALSE, 0)) {
        s_main_thread = NULL;
    }
    s_thread = CreateThread(NULL, 0, heartbeat_thread, NULL, 0, NULL);
    if (s_thread) s_started = 1;
#endif
}

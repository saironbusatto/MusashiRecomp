#include "overlay_compile_worker.h"
#include "code_provider.h"
#include "overlay_sljit.h"
#include "crc32.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

/* Dispatch-side publish (overlay_loader.c): atomically write the serialized
 * shard into the on-disk cache + flag the loader to rescan-on-miss. It touches
 * only the filesystem and read-only-after-init cache-path globals, so it is safe
 * to call from this worker thread (it never touches the candidate table). */
extern void overlay_loader_async_publish(uint32_t entry_phys, uint32_t lo,
                                         uint32_t len, uint32_t crc,
                                         const void *blob, unsigned long blob_size);

#define WQ_CAP     256          /* bounded work queue                          */
#define STATE_CAP  4096         /* per-(phys,crc) dedup map                    */
#define SNAP_MAX   (8u * 1024u) /* max window we JIT (SLJIT_MAX_FRAG_INSNS*4)  */

typedef struct {
    uint32_t vaddr, phys, crc, snap_len;
    uint8_t *snap;              /* worker-owned copy of the region bytes        */
} WorkItem;

enum { ST_QUEUED = 1, ST_COMPILING = 2, ST_DONE = 3, ST_FAILED = 4 };
typedef struct { uint32_t phys, crc; uint8_t state; } StateEnt;

static SDL_Thread *s_thread;
static SDL_mutex  *s_mtx;
static SDL_cond   *s_cv;
static int         s_running;

static WorkItem    s_q[WQ_CAP];
static int         s_q_head, s_q_tail, s_q_n;

static StateEnt    s_state[STATE_CAP];
static int         s_state_n;

static uint64_t    s_enq, s_done, s_failed;

/* Always-on recent-compile ring (CLAUDE.md ring-buffer doctrine): every compile,
 * success or decline, lands here so a probe can QUERY what the worker did in the
 * window of interest rather than time-and-capture. Guarded by s_mtx. */
#define EVT_CAP 64
static OverlayCompileEvent s_evt[EVT_CAP];
static uint64_t            s_evt_seq;          /* total events recorded ever       */
static uint32_t            s_last_ms, s_max_ms;

/* dedup map — all accesses hold s_mtx. */
static StateEnt *state_find(uint32_t phys, uint32_t crc) {
    for (int i = 0; i < s_state_n; i++)
        if (s_state[i].phys == phys && s_state[i].crc == crc) return &s_state[i];
    return NULL;
}
static StateEnt *state_add(uint32_t phys, uint32_t crc, uint8_t st) {
    if (s_state_n >= STATE_CAP) {
        /* Full: evict the first finished entry (the on-disk cache is the source
         * of truth, so a later re-miss can re-enqueue if ever needed). COMPILING/
         * QUEUED entries are never evicted (in flight). */
        for (int i = 0; i < s_state_n; i++)
            if (s_state[i].state == ST_DONE || s_state[i].state == ST_FAILED) {
                s_state[i] = s_state[--s_state_n];
                break;
            }
        if (s_state_n >= STATE_CAP) return NULL;
    }
    StateEnt *e = &s_state[s_state_n++];
    e->phys = phys; e->crc = crc; e->state = st;
    return e;
}

static int worker_main(void *unused) {
    (void)unused;
    const CodeProvider *cp = code_provider_sljit();
    SDL_LockMutex(s_mtx);
    for (;;) {
        while (s_running && s_q_n == 0) SDL_CondWait(s_cv, s_mtx);
        if (!s_running && s_q_n == 0) break;
        WorkItem it = s_q[s_q_head];
        s_q_head = (s_q_head + 1) % WQ_CAP;
        s_q_n--;
        { StateEnt *e = state_find(it.phys, it.crc); if (e) e->state = ST_COMPILING; }
        SDL_UnlockMutex(s_mtx);

        /* Compile off-thread FROM THE SNAPSHOT (base = phys, so the fragment's
         * byte offset into the buffer is entry_phys - phys). Never live RAM. */
        int ok = 0;
        uint32_t ev_lo = 0, ev_len = 0;
        Uint32   t0 = SDL_GetTicks();
        if (cp && cp->compile_fragment && it.snap) {
            CompiledFragment frag;
            memset(&frag, 0, sizeof frag);
            cp->compile_fragment(it.vaddr, it.snap, it.snap_len, it.phys, &frag);
            if (frag.fn && frag.serialized && frag.serialized_size && frag.code_len &&
                frag.code_lo >= it.phys &&
                (frag.code_lo - it.phys) + frag.code_len <= it.snap_len) {
                /* crc over the EXACT compiled range within the snapshot — this is
                 * what dispatch re-hashes live RAM against when it loads the shard
                 * (matches persist_sljit_shard's crc-over-code-range). */
                uint32_t off = frag.code_lo - it.phys;
                uint32_t crc = crc32_update(0xFFFFFFFFu, it.snap + off, frag.code_len)
                               ^ 0xFFFFFFFFu;
                overlay_loader_async_publish(it.phys, frag.code_lo, frag.code_len,
                                             crc, frag.serialized, frag.serialized_size);
                ev_lo = frag.code_lo; ev_len = frag.code_len;
                ok = 1;
            }
            if (frag.serialized) overlay_sljit_free_serialized(frag.serialized);
        }
        uint32_t dur = (uint32_t)(SDL_GetTicks() - t0);
        free(it.snap);

        SDL_LockMutex(s_mtx);
        { StateEnt *e = state_find(it.phys, it.crc); if (e) e->state = ok ? ST_DONE : ST_FAILED; }
        if (ok) s_done++; else s_failed++;
        /* record into the recent-compile ring */
        OverlayCompileEvent *ev = &s_evt[s_evt_seq % EVT_CAP];
        ev->vaddr = it.vaddr; ev->phys = it.phys; ev->crc = it.crc;
        ev->code_lo = ev_lo;  ev->code_len = ev_len;
        ev->compile_ms = dur; ev->ok = (uint8_t)ok;
        s_evt_seq++;
        s_last_ms = dur; if (dur > s_max_ms) s_max_ms = dur;
    }
    SDL_UnlockMutex(s_mtx);
    return 0;
}

void overlay_compile_worker_start(void) {
    if (s_thread) return;
    if (!s_mtx) s_mtx = SDL_CreateMutex();
    if (!s_cv)  s_cv  = SDL_CreateCond();
    if (!s_mtx || !s_cv) return;
    s_running = 1;
    s_thread = SDL_CreateThread(worker_main, "psx-sljit-compile", NULL);
    if (!s_thread) s_running = 0;
}

void overlay_compile_worker_stop(void) {
    if (!s_thread) return;
    SDL_LockMutex(s_mtx);
    s_running = 0;
    SDL_CondSignal(s_cv);
    SDL_UnlockMutex(s_mtx);
    SDL_WaitThread(s_thread, NULL);
    s_thread = NULL;
    SDL_LockMutex(s_mtx);
    while (s_q_n) { free(s_q[s_q_head].snap); s_q_head = (s_q_head + 1) % WQ_CAP; s_q_n--; }
    SDL_UnlockMutex(s_mtx);
}

void overlay_compile_worker_enqueue(uint32_t vaddr, uint32_t phys,
                                    const uint8_t *src, uint32_t snap_len,
                                    uint32_t crc) {
    if (!s_thread || !src || snap_len == 0) return;
    if (snap_len > SNAP_MAX) snap_len = SNAP_MAX;
    SDL_LockMutex(s_mtx);
    if (state_find(phys, crc) || s_q_n >= WQ_CAP) { SDL_UnlockMutex(s_mtx); return; }
    uint8_t *copy = (uint8_t *)malloc(snap_len);
    if (!copy) { SDL_UnlockMutex(s_mtx); return; }
    memcpy(copy, src, snap_len);
    if (!state_add(phys, crc, ST_QUEUED)) { free(copy); SDL_UnlockMutex(s_mtx); return; }
    WorkItem *it = &s_q[s_q_tail];
    it->vaddr = vaddr; it->phys = phys; it->crc = crc; it->snap_len = snap_len; it->snap = copy;
    s_q_tail = (s_q_tail + 1) % WQ_CAP;
    s_q_n++;
    s_enq++;
    SDL_CondSignal(s_cv);
    SDL_UnlockMutex(s_mtx);
}

void overlay_compile_worker_stats(uint64_t *enqueued, uint64_t *compiled,
                                  uint64_t *failed, uint32_t *queued_now) {
    if (s_mtx) SDL_LockMutex(s_mtx);
    if (enqueued)   *enqueued   = s_enq;
    if (compiled)   *compiled   = s_done;
    if (failed)     *failed     = s_failed;
    if (queued_now) *queued_now = (uint32_t)s_q_n;
    if (s_mtx) SDL_UnlockMutex(s_mtx);
}

int overlay_compile_worker_running(void) { return s_thread != NULL; }

void overlay_compile_worker_timing(uint32_t *last_ms, uint32_t *max_ms) {
    if (s_mtx) SDL_LockMutex(s_mtx);
    if (last_ms) *last_ms = s_last_ms;
    if (max_ms)  *max_ms  = s_max_ms;
    if (s_mtx) SDL_UnlockMutex(s_mtx);
}

int overlay_compile_worker_recent(OverlayCompileEvent *out, int cap) {
    if (!out || cap <= 0) return 0;
    if (s_mtx) SDL_LockMutex(s_mtx);
    uint64_t total = s_evt_seq;
    int avail = (total < (uint64_t)EVT_CAP) ? (int)total : EVT_CAP;
    if (avail > cap) avail = cap;
    /* chronological (oldest first): the oldest of the kept window is at
     * (seq - avail). */
    uint64_t start = total - (uint64_t)avail;
    for (int i = 0; i < avail; i++)
        out[i] = s_evt[(start + (uint64_t)i) % EVT_CAP];
    if (s_mtx) SDL_UnlockMutex(s_mtx);
    return avail;
}

#ifndef PSX_OVERLAY_COMPILE_WORKER_H
#define PSX_OVERLAY_COMPILE_WORKER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Off-main-thread sljit compile worker.
 *
 * The dispatch (guest-fiber) thread, on an sljit gap-fill miss, snapshots the
 * region bytes and ENQUEUES here instead of JIT'ing inline (which used to spike
 * the frame). A single background worker pops the queue, JITs the fragment FROM
 * THE SNAPSHOT (never live guest RAM), and writes the serialized shard into the
 * on-disk cache via overlay_loader_async_publish(). The dispatch thread picks
 * the shard up on a later miss (cache-dirty -> idempotent rescan -> register),
 * so the candidate table stays single-threaded: the worker NEVER touches it.
 *
 * Safety invariants (see docs/ASYNC_OVERLAY_COMPILE.md §3/§5):
 *  - candidate tables / sljit registry: dispatch thread only.
 *  - guest RAM: worker reads only the enqueue-time snapshot copy.
 *  - the sljit emitter's file-static scratch is safe because the worker is the
 *    SOLE runtime caller of the compiler (dispatch enqueues; selftest is init-time).
 */

/* Start/stop the worker. Idempotent. stop() signals + joins. */
void overlay_compile_worker_start(void);
void overlay_compile_worker_stop(void);

/* Enqueue a region for background JIT. `vaddr` is the VIRTUAL entry (carries
 * KSEG bits the guest uses for return_pc/jal targets). `src` points at the
 * live region bytes [phys .. phys+snap_len); enqueue COPIES them, so the caller
 * keeps ownership of `src`. `crc` is a content hash of the snapshot window,
 * used only for (phys,crc) dedup. Deduped: a QUEUED/COMPILING/DONE (phys,crc)
 * is ignored. Cheap no-op if the worker isn't running. */
void overlay_compile_worker_enqueue(uint32_t vaddr, uint32_t phys,
                                    const uint8_t *src, uint32_t snap_len,
                                    uint32_t crc);

/* Telemetry for the debug server (any out-ptr may be NULL). */
void overlay_compile_worker_stats(uint64_t *enqueued, uint64_t *compiled,
                                  uint64_t *failed, uint32_t *queued_now);

/* 1 if the worker thread is currently running. */
int overlay_compile_worker_running(void);

/* Last / worst single-compile wall time (ms, SDL_GetTicks resolution). The whole
 * point of the worker is that this time is paid OFF the dispatch thread, so a
 * nonzero max here alongside a flat dispatch-thread ce_profile is the proof. */
void overlay_compile_worker_timing(uint32_t *last_ms, uint32_t *max_ms);

/* Always-on ring of the most-recent compile results (one entry PER shard the
 * worker has produced or rejected). Query this for the window of interest —
 * never arm-then-capture. */
typedef struct {
    uint32_t vaddr;      /* virtual entry enqueued                     */
    uint32_t phys;       /* physical entry (dedup key)                 */
    uint32_t crc;        /* content crc of the enqueue-time snapshot   */
    uint32_t code_lo;    /* compiled range start (phys)                */
    uint32_t code_len;   /* compiled range length in bytes (0 if fail) */
    uint32_t compile_ms; /* wall time for this compile (off-thread)    */
    uint8_t  ok;         /* 1 = published a shard, 0 = emitter declined */
} OverlayCompileEvent;

/* Copy up to `cap` most-recent events into `out` in chronological order (oldest
 * first), returning the count copied. Thread-safe. */
int overlay_compile_worker_recent(OverlayCompileEvent *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_OVERLAY_COMPILE_WORKER_H */

#ifndef OVERLAY_LOADER_H
#define OVERLAY_LOADER_H

/* overlay_loader — A-1 runtime overlay DLL cache and dynamic dispatch.
 *
 * On CD DMA completion (overlay_capture_on_dma), the loader checks whether
 * a cached DLL exists for the overlay bytes.  If it does, it LoadLibrary's
 * the DLL, calls overlay_init() to wire callbacks, enumerates func_XXXXXXXX
 * exports, and registers each in the dynamic dispatch table.
 *
 * dirty_ram_dispatch calls overlay_loader_dispatch() before the interpreter,
 * so compiled overlay functions get priority over interpretation.
 */

#include "cpu_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called at game handoff to set the cache root directory and game ID.
 * cache_dir: absolute path to the cache root (e.g. "build-dev/cache")
 * game_id:   product code (e.g. "SCUS-94236") */
void overlay_loader_init(const char *cache_dir, const char *game_id);

/* Called from overlay_capture_on_dma after the capture-set insert.
 * Computes CRC32 of bytes, checks cache, loads DLL if present.
 * load_addr: physical RAM address, size/bytes: the transferred data. */
void overlay_loader_check_cache(uint32_t load_addr, uint32_t size,
                                const uint8_t *bytes);

/* Called from dirty_ram_dispatch before the interpreter.
 * Returns 1 and calls the compiled function if addr is registered,
 * 0 if not found (fall through to interpreter). */
int overlay_loader_dispatch(CPUState *cpu, uint32_t addr);

/* Returns number of functions currently registered in the dynamic table. */
int overlay_loader_registered_count(void);

/* Returns full loader state for TCP diagnostics. */
void overlay_loader_get_status(int *active, int *registered,
                               int *regions_checked,
                               char *cache_dir_out, int cache_dir_len,
                               char *game_id_out,   int game_id_len,
                               uint32_t *checked_out, int checked_max,
                               int *checked_written,
                               uint32_t *last_crc_out, int *last_file_found_out);

/* Most recent loader event string (DLL load success/failure). Surfaced via
 * the overlay_loader_status TCP command — no stderr logging (Rule 3). */
const char *overlay_loader_last_msg(void);

/* Inc3 §8.5: a write into the code range of a currently-executing native
 * overlay entry cannot be recovered lazily — that entry is permanently
 * blacklisted to the interpreter. Called from the psx_write_* store path
 * (memory.c) only when the written page is a watched overlay code page. */
void overlay_loader_active_write_check(uint32_t phys, uint32_t size);

/* Inc3 counters, surfaced via overlay_loader_status (field meanings remapped
 * to the per-entry model — see overlay_loader.c getters). */
void overlay_loader_get_counters(uint32_t *loads, uint32_t *invalidations,
                                 uint32_t *unregistered,
                                 uint64_t *disp_native, uint64_t *disp_interp,
                                 uint64_t *stale_blocked,
                                 uint32_t *last_write_pc,
                                 uint32_t *last_write_addr,
                                 uint32_t *last_write_size,
                                 int *regions, uint32_t *revalidations);

void overlay_loader_get_reload_debug(int *r0_valid, uint32_t *r0_writes,
                                     uint32_t *r0_fn_lo, uint32_t *r0_fn_hi,
                                     uint32_t *r0_crc_live,
                                     uint32_t *reval_attempts,
                                     uint32_t *reval_crc_miss,
                                     uint32_t *last_reval_crc);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_LOADER_H */

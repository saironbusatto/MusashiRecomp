#ifndef OVERLAY_CAPTURE_H
#define OVERLAY_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* overlay_capture — B-1 implementation of the overlay capture set.
 *
 * On every CD DMA completion into game-code RAM (dest < 0x1C0000), the runtime
 * calls overlay_capture_on_dma().  Each unique load_addr is stored exactly once
 * in a write-once set.  At clean process exit, overlay_capture_write_json()
 * writes overlay_captures.json next to the executable for user contribution.
 *
 * The set never overwrites: a second DMA to the same load_addr is a no-op.
 * This is correct because PSX overlays always load the same bytes to the same
 * address; recording once is sufficient.
 */

/* Call once at startup (before the run loop) with the exe directory.
 * Sets the output path for overlay_captures.json.  Capture is not yet
 * active — it activates automatically on the first post-game DMA transfer. */
void overlay_capture_set_out_dir(const char *out_dir);

/* Legacy init alias kept for compatibility; same as set_out_dir. */
void overlay_capture_init(const char *out_dir);

/* Enable/disable overlay capture. Off by default; main() enables it only when
 * the overlay cache is turned on in [runtime] config. When disabled,
 * overlay_capture_on_dma() and overlay_capture_write_json() are no-ops. */
void overlay_capture_set_enabled(int enabled);

/* Call from dma.c execute_ch3_cdrom() after every forward CH3 transfer with
 * load_start < 0x1C0000 and fntrace_is_game_started().
 * load_addr is the physical RAM destination, size is byte count,
 * bytes points to the RAM buffer at that address (already written by DMA). */
void overlay_capture_on_dma(uint32_t load_addr, uint32_t size,
                             const uint8_t *bytes);

/* Write overlay_captures.json to the directory supplied at init time.
 * Safe to call even if no overlays were captured (writes nothing).
 * Called from shutdown_runtime() in main.cpp. */
void overlay_capture_write_json(void);

/* Returns number of unique overlays captured so far. */
int overlay_capture_count(void);

/* Compute CRC32 of the DMA-time bytes for the region [region_start,
 * region_start+region_size).  Zero-filled for gaps between DMA blocks,
 * exactly as overlay_captures.json bytes_b64 is assembled.  Use this
 * in overlay_loader to build a consistent DLL filename without reading
 * live RAM (which has scatter-load gap contamination). */
uint32_t overlay_capture_get_region_crc(uint32_t region_start,
                                         uint32_t region_size);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_CAPTURE_H */

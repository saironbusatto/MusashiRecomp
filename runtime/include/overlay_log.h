#ifndef PSX_OVERLAY_LOG_H
#define PSX_OVERLAY_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at startup with the game's product ID (e.g. "SCUS-94236") and
 * the directory where game.toml lives. Creates logs/<game_id>/ if needed. */
void overlay_log_init(const char* game_id, const char* game_dir);

/* Call after each CD DMA-to-RAM transfer completes (forward direction only).
 * Computes CRC32 of the written range, deduplicates, and appends to
 * overlay_map.jsonl if this (crc32, addr, size) hasn't been seen before. */
void overlay_log_record(uint32_t phys_addr, uint32_t size_bytes);

#ifdef __cplusplus
}
#endif

#endif /* PSX_OVERLAY_LOG_H */

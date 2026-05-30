#include "overlay_log.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_compat(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_compat(p) mkdir(p, 0755)
#endif

/* Only track transfers above the main EXE text ceiling and large enough to
 * plausibly be code.  FMV streaming, XA audio, and small data packets all
 * come through CD DMA too — they are cheap to reject here. */
#define OVERLAY_PHYS_MIN  0x00080000u   /* below = main EXE / kernel / data */
#define OVERLAY_SIZE_MIN  4096u          /* below = streaming data, not code  */
#define RAM_SIZE          (2u * 1024u * 1024u)

static FILE* s_log_file = NULL;

/* Seen table: deduplicate on (addr, size). No hash needed — same addr+size
 * in Tomba always means the same overlay. */
#define SEEN_CAP 128
typedef struct { uint32_t addr; uint32_t size; } SeenEntry;
static SeenEntry s_seen[SEEN_CAP];
static int       s_seen_count = 0;

void overlay_log_init(const char* game_id, const char* game_dir) {
    if (!game_id || !game_id[0] || !game_dir) return;

    char buf[512];
    snprintf(buf, sizeof(buf), "%s/logs", game_dir);
    mkdir_compat(buf);
    snprintf(buf, sizeof(buf), "%s/logs/%s", game_dir, game_id);
    mkdir_compat(buf);

    char log_path[512];
    snprintf(log_path, sizeof(log_path),
             "%s/logs/%s/overlay_map.jsonl", game_dir, game_id);

    s_log_file = fopen(log_path, "a");
    if (s_log_file)
        fprintf(stdout, "overlay_log: %s\n", log_path);
    else
        fprintf(stderr, "overlay_log: cannot open %s\n", log_path);
}

void overlay_log_record(uint32_t phys_addr, uint32_t size_bytes) {
    if (!s_log_file) return;
    if (phys_addr < OVERLAY_PHYS_MIN) return;
    if (size_bytes < OVERLAY_SIZE_MIN) return;
    if (phys_addr + size_bytes > RAM_SIZE) return;

    for (int i = 0; i < s_seen_count; i++)
        if (s_seen[i].addr == phys_addr && s_seen[i].size == size_bytes) return;

    if (s_seen_count < SEEN_CAP) {
        s_seen[s_seen_count].addr = phys_addr;
        s_seen[s_seen_count].size = size_bytes;
        s_seen_count++;
    }

    fprintf(s_log_file, "{\"load_addr\":\"0x%08X\",\"size\":%u}\n",
            phys_addr, size_bytes);
    fflush(s_log_file);
    fprintf(stdout, "overlay_log: 0x%08X  %u bytes\n", phys_addr, size_bytes);
}

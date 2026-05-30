/* gpu.h — PS1 GPU hardware simulation (Phase 3).
 *
 * Implements GPUSTAT, GP0, GP1, and 1024x512 16-bit VRAM.
 * No rendering to screen — just correct hardware state transitions.
 */

#ifndef PSXRECOMP_GPU_H
#define PSXRECOMP_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     gpu_init(void);
uint32_t gpu_read_gpustat(void);   /* 0x1F801814 read */
uint32_t gpu_read_gpuread(void);   /* 0x1F801810 read */
void     gpu_write_gp0(uint32_t val);  /* 0x1F801810 write */
void     gpu_write_gp1(uint32_t val);  /* 0x1F801814 write */
void     gpu_set_gp0_source(uint32_t addr); /* diagnostic source for next GP0 word */
void     gpu_vblank_tick(void);        /* Toggle LCF, called at each simulated vblank */

/* Display presentation accessors (Phase 3). */
const uint16_t* gpu_get_vram(void);    /* Pointer to 1024x512 16-bit VRAM */

typedef struct {
    uint32_t display_x, display_y;     /* VRAM start of display area (GP1(05h)) */
    uint32_t width, height;            /* Derived from display mode + ranges */
    int      disabled;                 /* GP1(03h) display disable flag */
} GpuDisplayInfo;

void gpu_get_display_info(GpuDisplayInfo* out);
uint64_t gpu_get_gp0_count(void);  /* Total GP0 writes since init */
void gpu_get_gp0_stats(uint64_t* nop, uint64_t* fill, uint64_t* draw, uint64_t* env, uint64_t* copy);

typedef struct {
    uint32_t left, top, right, bottom;
    int32_t offset_x, offset_y;
} GpuDrawArea;
void gpu_get_draw_area(GpuDrawArea* out);
uint16_t gpu_vram_peek(int x, int y);

/* Shaded quad vertex capture (Phase 4.5 debug). */
typedef struct {
    int32_t vx[4], vy[4];
    uint32_t color[4];
} GpuSqCapEntry;
void gpu_arm_shaded_quad_capture(void);
int  gpu_get_shaded_quad_capture(const GpuSqCapEntry** out);

/* Per-frame GP0 command ring (always-on; query via debug server).
 * Each entry records the GP0 command header + up to 6 payload words
 * (longer commands like 0x3C shaded textured quad are truncated to 6).
 * Stamped with the s_frame_count value at issue time so a debug
 * client can pull all commands for any frame in the ring window. */
#define GPU_GP0_RING_MAX_WORDS 12
typedef struct {
    uint32_t frame;
    uint32_t seq;
    uint32_t src_addr;      /* RAM/MMIO source address of command header, if known */
    uint32_t pc;            /* g_debug_last_store_pc when command completes */
    uint8_t  opcode;
    uint8_t  n_words;       /* total command length; >MAX means truncated */
    uint16_t pad;
    uint32_t cmd[GPU_GP0_RING_MAX_WORDS];
} GpuGp0RingEntry;

uint64_t gpu_gp0_ring_total(void);
uint32_t gpu_gp0_ring_capacity(void);
uint32_t gpu_gp0_ring_max_words(void);
int      gpu_gp0_ring_dump_frame(uint32_t frame, GpuGp0RingEntry *out, int max_out);
void     gpu_gp0_ring_frame_span(uint32_t *out_oldest, uint32_t *out_newest);

/* Vblank presentation callback — called from gpu_vblank_tick(). */
typedef void (*gpu_vblank_cb)(void);
void gpu_set_vblank_callback(gpu_vblank_cb cb);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_GPU_H */

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
    int      depth24;                  /* GP1(08h) display depth flag: RGB888 scanout */
    int      disabled;                 /* GP1(03h) display disable flag */
} GpuDisplayInfo;

void gpu_get_display_info(GpuDisplayInfo* out);
void gpu_display_pixel_rgb(const GpuDisplayInfo* di, uint32_t x, uint32_t y,
                           uint8_t* r, uint8_t* g, uint8_t* b);
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo* di, uint32_t x, uint32_t y);
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

/* Present-time screen-colour model (see color_lut.h ScreenKind: 0=raw,
 * 1=crt, 2=composite, 3=trinitron). Config/launcher-driven; the PSX_SCREEN
 * env var, if set, overrides this. Default 0 (raw) is byte-identical. */
void gpu_set_screen_kind(int kind);

/* Widescreen proportion correction (active only when aspect != 4:3 and the
 * game's [widescreen] block opts in — see config_loader.h). Tagged
 * character/billboard prims are re-squashed around their projected anchor;
 * untagged SPRT prims (screen-space HUD/menus) around the display centre.
 * psx_ws_sprite_tag is the per-prim callback the recompiler emits at the
 * entry of each [widescreen] sprite_tag_funcs function. */
void gpu_ws_configure(int aspect_num, int aspect_den,
                      uint32_t sprite_anchor_addr, int hud_sprt_squash);
struct CPUState;
void psx_ws_sprite_tag(struct CPUState* cpu);
/* True when the current frame must present at native 4:3 (FMV video or a
 * full-2D menu/title screen), so the squash is suppressed and content drawn
 * pixel-native. The present path uses the same predicate to pillarbox. */
int  gpu_ws_present_native_43(void);
/* Per-side X cull-margin (screen/world units) emitted into the game's draw-
 * cull immediates by the recompiler ([widescreen.cull]); 0 unless stretching. */
int  psx_ws_x_margin(void);

/* Live widescreen state for diagnostics (TCP gpu_state). All pointers
 * optional. last_tag_frame/cur_frame let the caller see game_mode freshness. */
typedef struct {
    int      configured;        /* ws_xnum != ws_xden */
    int      active;            /* squash currently applied this frame */
    int      game_mode;         /* tagged char/billboard prim within 2 frames */
    int      present_native_43; /* frame presents pillarboxed 4:3 (FMV/full-2D) */
    int      x_margin;          /* psx_ws_x_margin() right now */
    int      xnum, xden;        /* squash factor */
    uint64_t cur_frame;
    uint32_t last_tag_frame;    /* frame of newest tagged prim */
} GpuWsDebug;
void gpu_ws_get_debug(GpuWsDebug* out);

/* Diagnostic: force psx_ws_x_margin() to return v (>=0) regardless of state,
 * or -1 to restore the normal computed margin. For live cull-margin sweeps. */
void gpu_ws_set_margin_override(int v);

/* Always-on draw-census ring: every drawn primitive records frame / source
 * addr / camera / first-vertex screen pos, so object spawn/despawn and edge
 * culls are observable in data. Dump frames [f0,f1] to a CSV file. */
int      gpu_ws_census_dump(uint32_t f0, uint32_t f1, const char *path);
void     gpu_ws_census_set(int on);
uint64_t gpu_ws_census_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_GPU_H */

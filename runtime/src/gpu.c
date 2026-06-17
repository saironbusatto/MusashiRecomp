/* gpu.c — PS1 GPU hardware simulation (Phase 3, Step 1).
 *
 * Implements:
 *   - GPUSTAT register with correct bit semantics
 *   - GP1 commands 00h-08h (reset, display config)
 *   - VRAM storage (1024x512 x 16-bit)
 *   - GP0 command write — ABORTS (not yet implemented)
 *   - GPUREAD — returns last latched value
 *
 * Reference: nocash PSX specs, DuckStation src/core/gpu.cpp
 */

#include "gpu.h"
#include "gpu_sw_renderer.h"
#include "gpu_render.h"
#include "crash_trace.h"
#include "debug_server.h"
#include "cpu_state.h"
#include "event_ring.h"
#include "color_lut.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- VRAM ---- */
static uint16_t vram[1024 * 512];

/* ---- GP0 state machine ---- */

typedef enum {
    GP0_IDLE,
    GP0_COLLECTING,
    GP0_VRAM_WRITE,
    GP0_POLYLINE_MONO,    /* collecting mono polyline vertices */
    GP0_POLYLINE_SHADED   /* collecting shaded polyline color+vertex pairs */
} Gp0State;

static Gp0State gp0_state;
static uint64_t gp0_write_count;
static uint64_t gp0_nop_count, gp0_fill_count, gp0_draw_count, gp0_env_count, gp0_copy_count;
static uint32_t gp0_cmd_buf[16];   /* max fixed-length command is 12 words */
static int      gp0_words_collected;
static int      gp0_words_needed;
static uint32_t gp0_next_source_addr = 0xFFFFFFFFu;
static uint32_t gp0_cmd_source_addr  = 0xFFFFFFFFu;

/* ---- Widescreen proportion correction --------------------------------------
 * Active only when [video] aspect_ratio != 4:3 AND the game's [widescreen]
 * block opts in (see config_loader.h). Two mechanisms, both driven from the
 * game.toml so nothing here is game-specific:
 *
 *  1. Tagged character/billboard prims. The recompiler emits a
 *     psx_ws_sprite_tag(cpu) call at the entry of each configured
 *     sprite-tag function ($a0 = prim pointer, scratchpad holds the prim's
 *     GTE-projected anchor). The game computes the prim's screen-pixel
 *     offsets/widths itself — which our GTE X-squash never sees — so tagged
 *     prims get every X re-squashed around their own anchor at execution
 *     time, restoring correct proportions on the stretched present.
 *
 *  2. Untagged textured rects (SPRT family). These never went through the
 *     GTE at all (HUD, menus — pure screen space), so they're squashed
 *     around the display centre, presenting at native proportions.
 *     Untextured TILEs are never touched: full-screen fades/flashes must
 *     keep covering the whole frame.
 *
 * The tag table is keyed by the prim's guest address (the DMA linked-list
 * walk reports each word's source address) and stamped with the frame
 * counter, so stale entries age out without an explicit clear. */
static int32_t  ws_xnum = 1, ws_xden = 1;   /* X squash factor; 1/1 = off */
static uint32_t ws_anchor_addr = 0;          /* scratchpad addr of anchor SXY */
static int      ws_hud_sprt = 0;             /* edge-anchor untagged HUD SPRTs */

/* Wide-aspect mode: 0 = off (4:3 identity), 1 = squash (legacy hack — compress
 * a wider FOV into the 320 frame, present stretched), 2 = native-wide (render
 * the wider FOV into an actually wider frame and present 1:1; the GTE is NOT
 * squashed — the frame is widened via draw_offset/draw_area and the display
 * read). ws_cfg_num/den hold the configured aspect that drives the wide extent
 * in native-wide mode. */
static int      ws_mode    = 0;
static int      ws_cfg_num = 4, ws_cfg_den = 3;

#define WS_TAG_BUCKETS 4096                  /* power of two */
#define WS_TAG_PROBES  8
#define WS_FMV_HYSTERESIS 30                 /* frames a colour MDEC decode pins 4:3 */
typedef struct { uint32_t key; uint32_t stamp; int32_t anchor_x; } WsTag;
static WsTag    ws_tags[WS_TAG_BUCKETS];
static uint32_t ws_last_tag_stamp = (uint32_t)-1000; /* frame of newest tag */
extern uint64_t s_frame_count;               /* defined in debug_server.c */
extern int      mdec_recently_active(uint32_t within_frames);  /* mdec.c */

static int ws_configured(void) { return ws_xnum != ws_xden; }

/* Any wide mode engaged (squash or native-wide). Drives the FMV/menu 4:3
 * pillarbox + the game-vs-2D detector, which both modes share. */
static int ws_engaged(void) { return ws_mode != 0; }

/* Forward decls: defined later but used by psx_ws_backdrop_x above them. */
static int32_t ws_scale_about(int32_t x, int32_t ax);
static int32_t ws_disp_w(void);

/* Gameplay vs full-2D screen. Character/billboard prims tag (psx_ws_sprite_tag)
 * within the last couple of frames => the actor render funnel is running =>
 * gameplay. A menu/title/save screen draws no tagged prims. The 2-frame window
 * makes this frame-stable regardless of per-prim ordering within a frame. */
static int ws_game_mode(void) {
    return (uint32_t)s_frame_count - ws_last_tag_stamp <= 2;
}

/* True when the current frame is presented at native 4:3 (NOT stretched), so
 * ALL squash must be suppressed and the content rendered pixel-native:
 *   - FMV video (24-bit, or streamed 15-bit colour MDEC), and
 *   - full-2D screens (menus/title/save — no character billboards this frame).
 * Coupling squash to this exact predicate keeps content and present in lock-
 * step: we squash IFF we stretch. Per-prim center-squash mangles composite 2D
 * UI (dialog boxes built from tiled cap/middle pieces), so such screens get
 * zero squash + a 4:3 pillarbox instead. The FMV check is cached per frame;
 * game_mode is a cheap live check. */
int gpu_ws_present_native_43(void) {
    if (!ws_engaged()) return 0;
    if (!ws_game_mode()) return 1;                 /* full-2D screen */
    static uint32_t fmv_frame_cache = 0xFFFFFFFFu;
    static int      fmv_cached = 0;
    uint32_t f = (uint32_t)s_frame_count;
    if (f != fmv_frame_cache) {
        fmv_frame_cache = f;
        GpuDisplayInfo di; gpu_get_display_info(&di);
        fmv_cached = di.depth24 || mdec_recently_active(WS_FMV_HYSTERESIS);
    }
    return fmv_cached;
}

/* Squash applies only when configured AND the frame is being stretched. */
static int ws_active(void) { return ws_configured() && !gpu_ws_present_native_43(); }

/* ----- Native-wide (mode 2) ------------------------------------------------
 * Render the wider FOV into an actually wider frame instead of squashing it.
 * Engaged on game frames only (FMV/menu frames pillarbox 4:3 like squash). The
 * frame grows symmetrically: each side by OFFSET display-pixels, total by
 * EXTRA = 2*OFFSET. OFFSET = round(W*(3*num-4*den)/(8*den)) where W is the live
 * display width — derived from the aspect so it generalises past 16:9 and
 * tracks display-mode changes; EXTRA is forced even so OFFSET is integral
 * (e.g. W=320 @ 16:9 -> OFFSET=53, EXTRA=106, frame 320 -> 426). The shift is
 * applied via the GPU draw_offset (so 3D and 2D move together and stay
 * aligned), the draw-area widens by EXTRA so the shifted content is not clipped
 * on the right, and the present widens the display read by EXTRA. 0 when
 * native-wide is inactive (4:3 / boot / squash mode / FMV / full-2D). */
int ws_native_wide_active(void) {
    return ws_mode == 2 && !gpu_ws_present_native_43();
}
static int ws_nw_offset(void) {
    if (!ws_native_wide_active()) return 0;
    int numr = 3 * ws_cfg_num - 4 * ws_cfg_den;   /* > 0 for aspects wider than 4:3 */
    if (numr <= 0) return 0;
    int w = (int)ws_disp_w();
    return (w * numr + 4 * ws_cfg_den) / (8 * ws_cfg_den);   /* round-to-nearest */
}
int ws_nw_extra(void) { return 2 * ws_nw_offset(); }

/* Per-side X cull margin in screen/world units (the game's draw classifier
 * works in objX-camX where 1 unit ~= 1 native-4:3 screen pixel). The squash
 * shows a half-view of 160/s pixels (s = squash factor = ws_xnum/ws_xden), so
 * the visible edge moves out by 160*(1/s - 1) = 160*(xden-xnum)/xnum. Widening
 * the cull window by this restores the original off-screen margin at the new
 * edge. 0 whenever squash is inactive (4:3/boot/menu/FMV) → original cull. */
/* Diagnostic override (8C): when >= 0, psx_ws_x_margin() returns this value
 * unconditionally so a probe can sweep the cull margin live (0 = force 4:3
 * cull while still stretching; large = over-draw) at a fixed camera position.
 * -1 = normal computed margin. */
static int ws_margin_override = -1;
void gpu_ws_set_margin_override(int v) { ws_margin_override = v; }

int psx_ws_x_margin(void) {
    if (ws_margin_override >= 0) return ws_margin_override;
    /* Native-wide: widen the world-space draw cull by the per-side reveal
     * (== the centering OFFSET in screen px) so the game SUBMITS the geometry
     * that previously fell outside the 4:3 cull window; the wide compositor then
     * rasterizes it into the revealed margins. Same recompiler emit sites as the
     * squash path ([widescreen.cull]); 0 at 4:3 so the cull stays byte-identical. */
    if (ws_native_wide_active()) return ws_nw_offset();
    if (!ws_active()) return 0;
    return (160 * (ws_xden - ws_xnum) + ws_xnum / 2) / ws_xnum;
}

/* Shared render-funnel screen-X cull widening ([widescreen.cull] auto_screen_x),
 * called identically by the gcc emit, the sljit JIT, and the interpreter so every
 * overlay execution path widens the same way. sx = the GTE screen-X as loaded by
 * the guest's lhu (low 16 bits significant); imm = the original bound (0x140 /
 * 0x141). Returns the sltiu verdict (1 = on-screen/keep). Sign-extends sx and
 * shifts by +margin so BOTH 16:9 margins pass; at 4:3 margin==0 so it reduces
 * bit-for-bit to the vanilla `(uint16)sx < imm`. */
int psx_ws_cull_sltiu(uint32_t sx, uint32_t imm) {
    int m = psx_ws_x_margin();
    return ((uint32_t)((int32_t)(int16_t)(uint16_t)sx + m)
            < (uint32_t)((int32_t)imm + 2 * m)) ? 1 : 0;
}

/* Detect the GTE per-vertex trivial-reject signature in a run of instruction
 * words: at least one `sltiu …,0x140/0x141` (width) AND one `sltiu …,0xE0/0xF1`
 * (height). Lets the sljit JIT + interpreter gate the cull-widening to real
 * render funnels (a lone sltiu 0x140 elsewhere must stay vanilla). Mirrors the
 * recompiler's func_has_screen_extent_cull. */
int psx_ws_func_has_screen_cull(const uint32_t *words, int n) {
    int has_x = 0, has_y = 0;
    for (int i = 0; i < n; i++) {
        uint32_t w = words[i];
        if ((w & 0xFC000000u) != 0x2C000000u) continue;  /* sltiu */
        uint32_t imm = w & 0xFFFFu;
        if (imm == 0x140 || imm == 0x141) has_x = 1;
        else if (imm == 0xE0 || imm == 0xF1) has_y = 1;
        if (has_x && has_y) return 1;
    }
    return 0;
}

/* Widescreen backdrop screen-X correction ([widescreen.backdrop] x_sites).
 * The parallax 2D backdrop layer (ocean/cloud/mountain/grass — overlay actor
 * handlers e.g. 0x801216BC) computes its screen-X in pure integer math
 * (screenX = (worldX - camX) >> parallax) and NEVER goes through the GTE, so
 * the GTE X-squash (gte_set_display_aspect) that gives 3D the wider 16:9 FOV
 * does not touch it. The recompiler emits this on each handler's final
 * screenX store so the backdrop is squashed by the SAME factor around the
 * screen centre: a far piece whose 4:3 screenX sat past the 320px edge (and
 * was GPU-clipped → the blue void / half-rectangles at the edges) is pulled
 * in to cover the revealed FOV. Identity at 4:3 / boot / FMV / full-2D (the
 * exact ws_active() predicate the GTE squash uses), so one build serves both.
 * x is the int16 screenX the handler was about to store. */
int psx_ws_backdrop_x(int x) {
    if (!ws_active()) return (int16_t)x;
    int32_t cx = ws_disp_w() / 2;                 /* screen centre (=160 @ 320) */
    return ws_scale_about((int16_t)x, cx);
}

/* Backdrop PRELOAD predicate + value substitution ([widescreen.cull]
 * auto_backdrop). The recompiler/sljit/interp detect each scrolling-backdrop
 * column-window generator (see ws_backdrop_detect.h) and route its window START
 * and END bounds through psx_ws_backdrop_value(). When native-wide is engaged we
 * WIDEN the camera-tracked window by the 16:9 reveal: the START (left) bound
 * moves left by `margin`, the END (right) bound moves right by `margin`, where
 * margin is the per-side reveal in COLUMNS. The window of window_cols columns
 * spans (at least) the full display width, so reveal_cols = window_cols *
 * reveal_px / display_px = window_cols * nw_offset / disp_w (+slack). This draws
 * ONLY the now-visible columns (no whole-row overdraw, no fixed cap), and the
 * generator's own low/high clamps still bound it at the level edges. Gated on
 * native-wide only (NOT squash: that path uses psx_ws_backdrop_x()). Returns
 * `orig` unchanged at 4:3 / squash / boot / FMV, so 4:3 stays byte-identical. */
int psx_ws_backdrop_preload(void) { return ws_native_wide_active(); }

/* Live-tunable widen amount (set via the `ws_backdrop_margin` debug command):
 *   <0  => WHOLE-ROW preload: START->0, END->extent-1 (max generous)
 *    0  => identity: no widening, native-wide still on (A/B the effect)
 *   >0  => widen the camera-tracked window by N columns each side (bounded)
 * Default is a safe-but-generous bounded value; dialed live while debugging so
 * the widen strategy can be tuned without a rebuild. */
int g_ws_bd_margin = 0;   /* column preload proven irrelevant to the void; default off */
/* Set to 1 by the interpreter around its psx_ws_backdrop_value call so the value
 * function does NOT also ring-note: the interp records a richer entry (live
 * extent / camera / DL count). Native cache-DLL calls leave it 0, so the value
 * function notes them itself -- that is how the ring sees native execution. */
int g_ws_bd_from_interp = 0;

uint32_t psx_ws_backdrop_value(uint32_t orig, int is_end, int window_cols) {
    if (!psx_ws_backdrop_preload()) return orig;   /* 4:3 -> byte-identical */
    if (window_cols < 0) window_cols = -window_cols;
    int      m   = g_ws_bd_margin;
    int      from_interp = g_ws_bd_from_interp;
    g_ws_bd_from_interp = 0;                        /* consume one-shot flag */
    uint32_t finalv;
    if (m < 0) {
        /* Whole-row: the generator's shared clamps (START<0 -> 0; END>=extent ->
         * extent-1) pin the loop to [0, extent-1]. Bounded because `extent` is a
         * byte and the DL count is a byte (row <= 255 cols). */
        enum { WS_BD_END_SENTINEL = 0x7FFF };
        finalv = is_end ? (uint32_t)WS_BD_END_SENTINEL : 0u;
    } else if (m == 0) {
        finalv = orig;                              /* effect off (still recorded) */
    } else {
        finalv = is_end ? (orig + (uint32_t)m) : (orig - (uint32_t)m);
    }
    /* Single chokepoint for every path (interp hook, native cache-DLL via the
     * overlay callback, sljit). The interp records its own richer entry, so skip
     * here when it set the flag; native-DLL calls (flag 0) are recorded here with
     * pc/extent/camera = 0 -- which is how the ring sees native execution. */
    if (!from_interp)
        psx_ws_backdrop_ring_note(0u, is_end ? 2 /*WS_BD_END*/ : 1 /*WS_BD_START*/,
                                  window_cols, orig, finalv, 0, 0, 0, 0u, 0u);
    return finalv;
}

/* ---- auto_backdrop diagnostic ring (always-on; see BACKDROP_PRELOAD.md) -----
 * Every backdrop-window rewrite (interp or native) is recorded here so the live
 * tile-row extent, the camera-tracked bounds, and exactly WHICH of a scene's
 * windows fire can be read back via the `ws_backdrop_ring` debug command --
 * query the ring for the window of interest, never pause/step. The recorder is
 * fed by the interpreter (which has the live CPU state: s7=extent, the DL count,
 * and the scratchpad camera-X) at the rewrite site. */
typedef struct {
    uint32_t frame, pc, orig, finalv, base, dl;
    int      kind, wcols, extent, camx, count;
} WsBdRingEnt;
#define WS_BD_RING_CAP 512
static WsBdRingEnt s_bd_ring[WS_BD_RING_CAP];
static uint32_t    s_bd_ring_seq = 0;

void psx_ws_backdrop_ring_note(uint32_t pc, int kind, int wcols, uint32_t orig,
                               uint32_t finalv, int extent, int camx, int count,
                               uint32_t base, uint32_t dl) {
    WsBdRingEnt *e = &s_bd_ring[s_bd_ring_seq & (WS_BD_RING_CAP - 1)];
    e->frame  = (uint32_t)s_frame_count; e->pc = pc; e->kind = kind; e->wcols = wcols;
    e->orig   = orig; e->finalv = finalv; e->extent = extent; e->camx = camx; e->count = count;
    e->base   = base; e->dl = dl;
    s_bd_ring_seq++;
}

int psx_ws_backdrop_ring_json(char *buf, int cap) {
    int n = (int)(s_bd_ring_seq < WS_BD_RING_CAP ? s_bd_ring_seq : WS_BD_RING_CAP);
    uint32_t start = s_bd_ring_seq - (uint32_t)n;
    int off = snprintf(buf, (size_t)cap, "\"seq\":%u,\"n\":%d,\"ents\":[", s_bd_ring_seq, n);
    for (int i = 0; i < n && off < cap - 200; i++) {
        WsBdRingEnt *e = &s_bd_ring[(start + (uint32_t)i) & (WS_BD_RING_CAP - 1)];
        off += snprintf(buf + off, (size_t)(cap - off),
            "%s{\"f\":%u,\"pc\":\"%08x\",\"k\":%d,\"wc\":%d,\"o\":%d,\"fin\":%d,\"ext\":%d,\"cam\":%d,\"cnt\":%d,\"base\":\"%08x\",\"dl\":\"%08x\"}",
            i ? "," : "", e->frame, e->pc, e->kind, e->wcols,
            (int)(int16_t)e->orig, (int)(int16_t)e->finalv, e->extent, e->camx, e->count,
            e->base, e->dl);
    }
    off += snprintf(buf + off, (size_t)(cap - off), "]");
    return off;
}

/* Backdrop store-site registry. The [widescreen.backdrop] x_sites are emitted
 * into native cache-DLL code by the recompiler, but overlay code very often
 * runs INTERPRETED (no DLL loaded), where the emit can't reach. So the runtime
 * also registers the same site PCs here and the dirty-RAM interpreter applies
 * psx_ws_backdrop_x() at those `sh` PCs — same transform, both paths. Tiny set;
 * a linear scan per matching SH is negligible. */
#define WS_BACKDROP_SITES_MAX 16
static uint32_t ws_backdrop_sites[WS_BACKDROP_SITES_MAX];
static int      ws_backdrop_site_n = 0;

void psx_ws_set_backdrop_sites(const uint32_t* pcs, int n) {
    ws_backdrop_site_n = 0;
    if (!pcs) return;
    for (int i = 0; i < n && ws_backdrop_site_n < WS_BACKDROP_SITES_MAX; i++)
        ws_backdrop_sites[ws_backdrop_site_n++] = pcs[i] & 0x1FFFFFFFu;
}

int psx_ws_is_backdrop_site(uint32_t pc) {
    if (!ws_backdrop_site_n) return 0;
    pc &= 0x1FFFFFFFu;
    for (int i = 0; i < ws_backdrop_site_n; i++)
        if (ws_backdrop_sites[i] == pc) return 1;
    return 0;
}

/* Snapshot live widescreen state for the TCP gpu_state diagnostic. Mirrors
 * the predicates above so a probe can see, mid-gameplay, whether the squash
 * and the cull-margin are actually engaged (8C). */
void gpu_ws_get_debug(GpuWsDebug* out) {
    if (!out) return;
    out->configured        = ws_configured();
    out->active            = ws_active();
    out->game_mode         = ws_game_mode();
    out->present_native_43 = gpu_ws_present_native_43();
    out->x_margin          = psx_ws_x_margin();
    out->xnum              = ws_xnum;
    out->xden              = ws_xden;
    out->mode              = ws_mode;
    out->nw_extra          = ws_nw_extra();
    out->cur_frame         = s_frame_count;
    out->last_tag_frame    = ws_last_tag_stamp;
}

void gpu_ws_configure(int aspect_num, int aspect_den,
                      uint32_t sprite_anchor_addr, int hud_sprt_squash, int mode) {
    ws_cfg_num = aspect_num > 0 ? aspect_num : 4;
    ws_cfg_den = aspect_den > 0 ? aspect_den : 3;
    ws_mode    = mode;
    if (mode == 1) {
        /* Squash factor = (4*den)/(3*num) — the same factor the GTE applies. */
        int32_t n = 4 * ws_cfg_den, d = 3 * ws_cfg_num;
        int32_t a = n, b = d;
        while (b) { int32_t t = a % b; a = b; b = t; }
        ws_xnum = n / a;
        ws_xden = d / a;
    } else {
        /* Native-wide (2) and off (0): the GTE is NOT squashed. */
        ws_xnum = ws_xden = 1;
    }
    ws_anchor_addr = sprite_anchor_addr;
    ws_hud_sprt    = hud_sprt_squash;
}

/* Called from generated code at the entry of each [widescreen]
 * sprite_tag_funcs function: record prim pointer ($a0) -> projected anchor.
 * Gated on ws_configured (NOT ws_active): ws_active depends on game_mode which
 * THIS function sets, so gating it on ws_active would never let gameplay
 * start (the first character of a frame would be suppressed forever). */
void psx_ws_sprite_tag(CPUState* cpu) {
    if (!ws_engaged() || !ws_anchor_addr) return;
    uint32_t key = cpu->gpr[4] & 0x1FFFFCu;
    if (!key) return;
    uint32_t sxy = cpu->read_word(ws_anchor_addr);
    int32_t  ax  = (int32_t)(int16_t)(sxy & 0xFFFFu);
    uint32_t now = (uint32_t)s_frame_count;
    uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
    uint32_t victim = idx;
    for (int i = 0; i < WS_TAG_PROBES; i++) {
        uint32_t j = (idx + i) & (WS_TAG_BUCKETS - 1);
        WsTag *t = &ws_tags[j];
        if (t->key == key || t->key == 0) { victim = j; break; }
        if (now - t->stamp > 2) victim = j;  /* stale — reusable */
    }
    ws_tags[victim].key      = key;
    ws_tags[victim].stamp    = now;
    ws_tags[victim].anchor_x = ax;
    ws_last_tag_stamp = now;
}

/* Look up the executing GP0 command's prim in the tag table. The command's
 * first word lives at prim+4 (the PsyQ P_TAG header precedes it), but accept
 * a direct hit too in case a tag site passes the colour-word address. */
static int ws_tagged_anchor(int32_t *out_ax) {
    if (!ws_active() || gp0_cmd_source_addr == 0xFFFFFFFFu) return 0;
    uint32_t now = (uint32_t)s_frame_count;
    for (int variant = 0; variant < 2; variant++) {
        uint32_t key = (gp0_cmd_source_addr - (variant ? 0u : 4u)) & 0x1FFFFCu;
        uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
        for (int i = 0; i < WS_TAG_PROBES; i++) {
            WsTag *t = &ws_tags[(idx + i) & (WS_TAG_BUCKETS - 1)];
            if (t->key == key && now - t->stamp <= 2) {
                *out_ax = t->anchor_x;
                return 1;
            }
        }
    }
    return 0;
}

/* Native-wide-compatible "is the current GP0 prim sprite-tagged?" — same ws_tags
 * lookup as ws_tagged_anchor but gated on ws_engaged() (squash OR native-wide)
 * instead of ws_active() (squash only). Used by the GL native-wide 2D-backdrop
 * stretch to EXCLUDE foreground sprites: characters / Tomba / HUD are tagged by
 * psx_ws_sprite_tag (their prim ptr -> ws_tags), the 2D backdrop tiles are not.
 * Reads the live gp0_cmd_source_addr (the source addr of the prim being drawn). */
/* Flower-field backdrop data-structure address range — set by the dirty-RAM
 * interpreter when the backdrop generator (overlay 0x80116808) runs; its tile
 * packets live in [lo, hi]. The GL native-wide 2D-stretch gate matches the prim
 * being drawn (gp0_cmd_source_addr) against this to PRECISELY identify the
 * flower-field tiles — the 3D rock/foreground is untagged AND has narrow prims,
 * so tag/narrow heuristics alone mis-stretch and tear it. */
uint32_t g_ws_backdrop_lo = 0, g_ws_backdrop_hi = 0;
/* diag: per-frame min/max of the prim source addrs the GL gate sees */
uint32_t g_bdg_src_lo = 0xFFFFFFFFu, g_bdg_src_hi = 0;
static uint32_t bdg_src_frame = 0xFFFFFFFFu;

/* ---- prim<->pixel correlation debug gate (ws_dbg_stretch command) ------------
 * To identify WHICH drawn prims constitute the native-wide 2D-backdrop void layer
 * (vs Tomba/HUD/3D), this lets a probe stretch an exact SELECTABLE prim set and
 * screenshot the result. When g_dbg_mode != 0 the GL stretch gate uses this
 * instead of the flower-struct range. Modes (buffer/frame-independent except 1):
 *   1 OT-range   : gp0_cmd_source_addr in [g_dbg_lo,g_dbg_hi]
 *   2 clut       : textured prim whose clut == g_dbg_clut
 *   3 tpage      : textured prim whose tpage == (g_dbg_clut & 0x1FF)
 *   4 tagged     : prim is sprite-tagged (psx_ws_prim_is_tagged)
 *   5 untag-tex  : textured AND not sprite-tagged
 *   6 all-tex    : every textured prim (sanity: should fill void + distort 3D)
 * Counters (per-frame, snapshotted by the present hook) report how many prims
 * matched and how many of those were sprite-tagged. */
int      g_dbg_mode = 0;
uint32_t g_dbg_lo = 0, g_dbg_hi = 0;
uint16_t g_dbg_clut = 0;
int      g_dbg_match_n = 0, g_dbg_match_tagged = 0;        /* snapshot (reported) */
static int s_dbg_match_n = 0, s_dbg_match_tagged = 0;      /* accumulate per frame */
int psx_ws_prim_is_tagged(void);   /* defined below */

static int dbg_gate_match(void) {
    uint32_t op   = (gp0_cmd_buf[0] >> 24) & 0xFFu;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint32_t a    = gp0_cmd_source_addr & 0x1FFFFFFFu;
    int textured  = ((op >= 0x20 && op <= 0x3F) && (op & 0x04)) || (op >= 0x64 && op <= 0x67);
    int tagged    = psx_ws_prim_is_tagged();
    int m = 0;
    switch (g_dbg_mode) {
        case 1: m = (g_dbg_hi > g_dbg_lo && a >= g_dbg_lo && a <= g_dbg_hi); break;
        case 2: m = (textured && clut == g_dbg_clut); break;
        case 3: m = (textured && ((uint16_t)(gp0_cmd_buf[4] >> 16) & 0x1FF) == (g_dbg_clut & 0x1FF)); break;
        case 4: m = tagged; break;
        case 5: m = (textured && !tagged); break;
        case 6: m = textured; break;
        default: m = 0; break;
    }
    if (m) { s_dbg_match_n++; if (tagged) s_dbg_match_tagged++; }
    return m;
}

/* Called by the present hook each frame to publish the dbg match counters. */
void psx_ws_dbg_gate_frame_snapshot(void) {
    g_dbg_match_n = s_dbg_match_n; g_dbg_match_tagged = s_dbg_match_tagged;
    s_dbg_match_n = 0; s_dbg_match_tagged = 0;
}

int psx_ws_prim_in_backdrop(void) {
    if (gp0_cmd_source_addr != 0xFFFFFFFFu) {
        uint32_t f = (uint32_t)s_frame_count;
        if (f != bdg_src_frame) { bdg_src_frame = f; g_bdg_src_lo = 0xFFFFFFFFu; g_bdg_src_hi = 0; }
        if (gp0_cmd_source_addr < g_bdg_src_lo) g_bdg_src_lo = gp0_cmd_source_addr;
        if (gp0_cmd_source_addr > g_bdg_src_hi) g_bdg_src_hi = gp0_cmd_source_addr;
    }
    if (g_dbg_mode != 0) return dbg_gate_match();   /* correlation override */
    if (g_ws_backdrop_hi <= g_ws_backdrop_lo || gp0_cmd_source_addr == 0xFFFFFFFFu) return 0;
    uint32_t a = gp0_cmd_source_addr & 0x1FFFFFFFu;
    return a >= (g_ws_backdrop_lo & 0x1FFFFFFFu) && a <= (g_ws_backdrop_hi & 0x1FFFFFFFu);
}

int psx_ws_prim_is_tagged(void) {
    if (!ws_engaged() || gp0_cmd_source_addr == 0xFFFFFFFFu) return 0;
    uint32_t now = (uint32_t)s_frame_count;
    for (int variant = 0; variant < 2; variant++) {
        uint32_t key = (gp0_cmd_source_addr - (variant ? 0u : 4u)) & 0x1FFFFCu;
        uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
        for (int i = 0; i < WS_TAG_PROBES; i++) {
            WsTag *t = &ws_tags[(idx + i) & (WS_TAG_BUCKETS - 1)];
            if (t->key == key && now - t->stamp <= 2) return 1;
        }
    }
    return 0;
}

/* Squash x around pivot ax by ws_xnum/ws_xden, round to nearest. */
static int32_t ws_scale_about(int32_t x, int32_t ax) {
    int32_t d = x - ax;
    int32_t s = (d * ws_xnum + (d >= 0 ? ws_xden / 2 : -ws_xden / 2)) / ws_xden;
    return ax + s;
}

/* Squash a width; never below 1px so nothing vanishes. */
static int32_t ws_scale_len(int32_t w) {
    int32_t s = (w * ws_xnum + ws_xden / 2) / ws_xden;
    return s < 1 ? 1 : s;
}

/* Horizontal display width in drawing space (e.g. 320). */
static int32_t ws_disp_w(void) {
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    return di.width ? (int32_t)di.width : 320;
}

/* In-game HUD pivot for an untagged screen-space SPRT spanning [x, x+w).
 * Only reached during gameplay (ws_active true). HUD elements are sparse and
 * corner-anchored, so pivot by thirds: outer-third elements anchor to their
 * screen edge (keeping the wide-screen corner position at native proportions),
 * the middle third to centre. A composite (counter box + digits) sits inside
 * one zone, so its pieces share a pivot and stay aligned. (Full-2D menu
 * screens never reach here — they get zero squash + 4:3 pillarbox instead.) */
static int32_t ws_hud_pivot(int32_t x, int32_t w) {
    int32_t W = ws_disp_w();
    int32_t cx = 2 * x + w;            /* 2*centre, avoids losing the half */
    if (3 * cx < 2 * W) return 0;
    if (3 * cx > 4 * W) return W;
    return W / 2;
}

/* Shared transform for fixed-size textured sprites (8x8 / 16x16 / 1x1 dot):
 * squash *x0 in place (around the tagged anchor, else the HUD pivot) and
 * return the squashed draw width, or 0 = no change. */
static int ws_sprt_fixed_transform(int32_t *x0, int w) {
    if (!ws_active()) return 0;
    int32_t ax;
    if (ws_tagged_anchor(&ax)) {
        *x0 = ws_scale_about(*x0, ax);
        return (int)ws_scale_len(w);
    }
    if (ws_hud_sprt) {
        *x0 = ws_scale_about(*x0, ws_hud_pivot(*x0, w));
        return (int)ws_scale_len(w);
    }
    return 0;
}

/* Polyline state */
static uint16_t polyline_color;       /* mono polyline: current color */
static int32_t  polyline_prev_x, polyline_prev_y;  /* previous vertex */
static uint16_t polyline_prev_c;      /* shaded polyline: previous color */
static int      polyline_semi_trans;  /* semi-transparency flag from command word */
static int      polyline_has_prev;    /* have we seen at least one vertex? */

/* VRAM write transfer state (CPU→VRAM, command 0xA0) */
static uint16_t vram_write_x, vram_write_y;   /* start coords */
static uint16_t vram_write_w, vram_write_h;   /* dimensions */
static uint16_t vram_write_col, vram_write_row; /* current offset */
static uint32_t vram_write_remaining;          /* words remaining */

/* VRAM read transfer state (VRAM→CPU, command 0xC0) */
static int      vram_read_active;
static uint16_t vram_read_x, vram_read_y;
static uint16_t vram_read_w, vram_read_h;
static uint16_t vram_read_col, vram_read_row;

/* ---- GPU internal state ---- */

/* Texture page / draw mode (set by GP0(E1h), reflected in GPUSTAT bits 0-10) */
static uint32_t texpage_x;         /* bits 0-3: texture page X base (N*64) */
static uint32_t texpage_y;         /* bit 4: texture page Y base (0 or 256) */
static uint32_t semi_transparency; /* bits 5-6 */
static uint32_t texpage_colors;    /* bits 7-8: 4bit/8bit/15bit */
static uint32_t dither_enabled;    /* bit 9 */
static uint32_t draw_to_display;   /* bit 10: drawing to display area allowed */
static uint32_t set_mask_bit;      /* bit 11 */
static uint32_t check_mask_bit;    /* bit 12 */
static uint32_t interlace_field;   /* bit 13 */
static uint32_t reverse_flag;      /* bit 14 */
static uint32_t texture_disable;   /* bit 15 */

/* Draw area (set by GP0(E3h)/GP0(E4h)) */
static uint32_t draw_area_left, draw_area_top;
static uint32_t draw_area_right, draw_area_bottom;

/* Draw offset (set by GP0(E5h)) */
static int32_t draw_offset_x, draw_offset_y;

/* Texture window raw value (set by GP0(E2h), readback via GP1(10h)) */
static uint32_t texture_window_value;

/* Display mode (set by GP1(08h), reflected in GPUSTAT bits 16-22) */
static uint32_t hres2;            /* bit 16: horizontal resolution 2 (368 mode) */
static uint32_t hres1;            /* bits 17-18: horizontal resolution 1 */
static uint32_t vres;             /* bit 19: vertical resolution (0=240, 1=480) */
static uint32_t video_mode;       /* bit 20: 0=NTSC, 1=PAL */
static uint32_t display_depth;    /* bit 21: 0=15bit, 1=24bit */
static uint32_t vertical_interlace; /* bit 22 */

/* Display enable (set by GP1(03h)) */
static uint32_t display_disabled; /* bit 23: 0=enabled, 1=disabled */

/* IRQ1 (set by GP0(1Fh), acked by GP1(02h)) */
static uint32_t irq1_flag;       /* bit 24 */

/* DMA direction (set by GP1(04h)) */
static uint32_t dma_direction;   /* bits 29-30 in GPUSTAT */

/* LCF (even/odd line in interlace, toggles per vblank) */
static uint32_t lcf;             /* bit 31 */

/* I_STAT — defined in memory.c; declared early so gpu_read_gpustat can
 * raise the VBLANK bit when a synthetic vblank fires from a tight
 * BIOS-shell GPUSTAT poll loop, without going through the
 * vblank-callback path (which would call SDL_Delay/Sleep). */
extern uint32_t i_stat;

/* Display area start (GP1(05h)) */
static uint32_t display_area_x;
static uint32_t display_area_y;

/* ----- Native-wide compositor driving (see runtime/src/gpu_sw_renderer.c) ----
 * The renderer keeps a separate wide surface per framebuffer; we tell it which
 * back buffer each draw targets and present from the displayed buffer's surface.
 * A "framebuffer base" is a VRAM x-origin the display has scanned out from — we
 * learn that set from GP1(05h) so the mirror only engages for real display
 * buffers (not offscreen texture builds). Generic: no per-game constants. */
#define WS_FB_BASES 4
static uint32_t ws_fb_base[WS_FB_BASES];
static int      ws_fb_n = 0;
static void ws_note_display_base(uint32_t bx) {
    for (int i = 0; i < ws_fb_n; i++) if (ws_fb_base[i] == bx) return;
    if (ws_fb_n < WS_FB_BASES) { ws_fb_base[ws_fb_n++] = bx; return; }
    for (int i = 1; i < WS_FB_BASES; i++) ws_fb_base[i - 1] = ws_fb_base[i];
    ws_fb_base[WS_FB_BASES - 1] = bx;
}
static int ws_is_fb_base(uint32_t bx) {
    for (int i = 0; i < ws_fb_n; i++) if (ws_fb_base[i] == bx) return 1;
    return 0;
}
/* Point the renderer's wide mirror at the current back buffer (draw_area_left)
 * when native-wide is active and that buffer is a known display buffer; else
 * disable mirroring for this draw. Called when the draw env changes. */
static void ws_nw_sync_target(void) {
    if (!ws_native_wide_active()) { gr_wide_disable_target(); return; }
    gr_wide_configure((int)ws_disp_w() + ws_nw_extra(), ws_nw_offset());
    uint32_t base = draw_area_left;
    if (ws_is_fb_base(base)) gr_wide_set_target((int)base);
    else                     gr_wide_disable_target();
}

/* Horizontal display range (GP1(06h)) */
static uint32_t h_display_x1;
static uint32_t h_display_x2;

/* Vertical display range (GP1(07h)) */
static uint32_t v_display_y1;
static uint32_t v_display_y2;

/* GPUREAD latch (GP1(10h) get-info result, or VRAM read data) */
static uint32_t gpuread_latch;

/* C0 (VRAM→CPU) capture slot — forward declaration for gpu_read_gpuread */
#define C0_HISTORY_CAP_FWD 32
static struct C0HistEntry {
    uint16_t x, y, w, h;
    uint32_t func_addr, sp_val, s1_val;
    uint32_t first_words[4];
    int read_count;
} c0_history_fwd[C0_HISTORY_CAP_FWD];
static int c0_history_count_fwd = 0;
static int c0_capture_slot_fwd = -1;

/* Vblank presentation callback */
static gpu_vblank_cb vblank_callback;

/* Shaded quad vertex capture (Phase 4.5 debug) — forward declarations
 * so gpu_vblank_tick can reference sq_cap_armed. */
#define SQ_CAP_MAX 32
static GpuSqCapEntry sq_cap_buf[SQ_CAP_MAX];
static int sq_cap_count;
static int sq_cap_armed;

/* GPUSTAT poll counter: in v4, recompiled code runs as native C without
 * per-instruction stepping. The BIOS contains tight VSYNC wait loops that
 * poll GPUSTAT bit 31 (LCF) waiting for it to toggle. LCF only changes
 * via gpu_vblank_tick(), which normally fires from psx_check_interrupts()
 * at dispatch boundaries. A polling loop within a single recompiled
 * function would spin forever.
 *
 * Fix: count GPUSTAT reads and trigger a vblank when the BIOS has polled
 * enough times, approximating the real hardware timing where the field
 * flips every ~33,868 GPU clocks (one NTSC frame). */
#define GPUSTAT_POLL_VBLANK_THRESHOLD 1000
static uint32_t gpustat_poll_count;

/* ---- Initialization ---- */

void gpu_init(void) {
    memset(vram, 0, sizeof(vram));
    gr_init(vram);

    /* Reset GP0 state machine */
    gp0_state = GP0_IDLE;
    gp0_words_collected = 0;
    gp0_words_needed = 0;
    vram_write_remaining = 0;
    vram_read_active = 0;

    /* Reset all state to power-on defaults */
    texpage_x = 0;
    texpage_y = 0;
    semi_transparency = 0;
    texpage_colors = 0;
    dither_enabled = 0;
    draw_to_display = 0;
    set_mask_bit = 0;
    check_mask_bit = 0;
    interlace_field = 0;
    reverse_flag = 0;
    texture_disable = 0;

    draw_area_left = 0;
    draw_area_top = 0;
    draw_area_right = 0;
    draw_area_bottom = 0;
    draw_offset_x = 0;
    draw_offset_y = 0;
    texture_window_value = 0;

    hres2 = 0;
    hres1 = 0;
    vres = 0;
    video_mode = 0;
    display_depth = 0;
    vertical_interlace = 0;

    display_disabled = 1;  /* display is OFF after reset */
    irq1_flag = 0;
    dma_direction = 0;
    lcf = 0;

    display_area_x = 0;
    display_area_y = 0;
    h_display_x1 = 0x200;
    h_display_x2 = 0xC00;
    v_display_y1 = 0x010;
    v_display_y2 = 0x100;

    gpuread_latch = 0;
    gpustat_poll_count = 0;
}

/* ---- GPUSTAT read (0x1F801814) ---- */

uint32_t gpu_read_gpustat(void) {
    /* Advance vblank when polled enough times from within a single function.
     * This handles BIOS VSYNC wait loops that poll LCF in tight loops.
     *
     * IMPORTANT: only update emulation state here (LCF + I_STAT). Do NOT
     * fire the vblank callback — that calls sdl_vblank_present, which
     * runs SDL_RenderPresent and SDL_Delay (Sleep). Entering Sleep from
     * an MMIO-read code path means recompiled MIPS code spends real wall
     * time inside Sleep — wrong context, hard to reason about, and
     * accumulates host stack frames inside the recompiled call tree.
     * The callback fires from the proper VBLANK trigger in
     * psx_check_interrupts (cycle-paced). */
    gpustat_poll_count++;
    if (gpustat_poll_count >= GPUSTAT_POLL_VBLANK_THRESHOLD) {
        gpustat_poll_count = 0;
        lcf ^= 1;
        i_stat |= (1u << 0); /* IRQ_VBLANK */
        /* DEQUEUE: VBlank fired via the GPUSTAT-poll fallback path (distinct
         * from the cycle-paced VBlank in psx_check_interrupts). */
        event_ring_record_aux(EV_DEQ, (uint8_t)SRC_VBLANK, 0xFFFFFFFFu);
    }

    uint32_t stat = 0;

    stat |= (texpage_x & 0xF);
    stat |= (texpage_y & 1) << 4;
    stat |= (semi_transparency & 3) << 5;
    stat |= (texpage_colors & 3) << 7;
    stat |= (dither_enabled & 1) << 9;
    stat |= (draw_to_display & 1) << 10;
    stat |= (set_mask_bit & 1) << 11;
    stat |= (check_mask_bit & 1) << 12;
    stat |= (interlace_field & 1) << 13;
    stat |= (reverse_flag & 1) << 14;
    stat |= (texture_disable & 1) << 15;
    stat |= (hres2 & 1) << 16;
    stat |= (hres1 & 3) << 17;
    stat |= (vres & 1) << 19;
    stat |= (video_mode & 1) << 20;
    stat |= (display_depth & 1) << 21;
    stat |= (vertical_interlace & 1) << 22;
    stat |= (display_disabled & 1) << 23;
    stat |= (irq1_flag & 1) << 24;

    /* Bit 25: DMA request — depends on DMA direction.
     * Direction 0: always 0
     * Direction 1: FIFO not full (always 1 for now — we process instantly)
     * Direction 2: same as bit 28 (ready to receive DMA block)
     * Direction 3: same as bit 27 (ready to send VRAM to CPU)
     */
    switch (dma_direction) {
        case 0: break; /* bit 25 = 0 */
        case 1: stat |= (1u << 25); break;
        case 2: stat |= (1u << 25); break; /* mirrors ready-to-receive */
        case 3: stat |= (1u << 25); break; /* mirrors ready-to-send */
    }

    /* Bit 26: ready to receive cmd word — 1 when not busy */
    stat |= (1u << 26);

    /* Bit 27: ready to send VRAM to CPU — 1 when VRAM read is active */
    if (vram_read_active)
        stat |= (1u << 27);

    /* Bit 28: ready to receive DMA block — 1 when not busy */
    stat |= (1u << 28);

    /* Bits 29-30: DMA direction */
    stat |= (dma_direction & 3) << 29;

    /* Bit 31: LCF — drawing even/odd lines in interlace mode */
    stat |= (lcf & 1) << 31;

    return stat;
}

/* ---- GPUREAD (0x1F801810 read) ---- */

uint32_t gpu_read_gpuread(void) {
    if (!vram_read_active)
        return gpuread_latch;

    /* Read two 16-bit pixels from VRAM and pack into one 32-bit word.
     * Routed through the renderer facade: under the GL backend the GPU-side
     * framebuffer is authoritative and must be synced down before the CPU
     * array is read (no-op cost on the software backend). */
    uint32_t value = 0;
    for (int i = 0; i < 2; i++) {
        uint16_t rx = (vram_read_x + vram_read_col) % 1024;
        uint16_t ry = (vram_read_y + vram_read_row) % 512;
        value |= (uint32_t)gr_vram_read((int)rx, (int)ry) << (i * 16);

        if (++vram_read_col == vram_read_w) {
            vram_read_col = 0;
            if (++vram_read_row == vram_read_h) {
                /* Transfer complete */
                vram_read_active = 0;
                break;
            }
        }
    }

    /* Capture first words for C0 debug */
    if (c0_capture_slot_fwd >= 0 && c0_capture_slot_fwd < C0_HISTORY_CAP_FWD) {
        int rc = c0_history_fwd[c0_capture_slot_fwd].read_count++;
        if (rc < 4)
            c0_history_fwd[c0_capture_slot_fwd].first_words[rc] = value;
        if (!vram_read_active)
            c0_capture_slot_fwd = -1;  /* transfer complete */
    }

    gpuread_latch = value;
    return value;
}

/* ---- GP0 command helpers ---- */

/* Convert 24-bit RGB888 to 16-bit RGB555 (PS1 VRAM format) */
static uint16_t rgb888_to_rgb555(uint32_t color24) {
    uint32_t r = (color24 >>  0) & 0xFF;
    uint32_t g = (color24 >>  8) & 0xFF;
    uint32_t b = (color24 >> 16) & 0xFF;
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

/* i_stat extern declared earlier in this file (above gpu_read_gpustat). */

void gpu_vblank_tick(void) {
    lcf ^= 1;
    gpustat_poll_count = 0;
    i_stat |= (1u << 0); /* IRQ_VBLANK */
    if (vblank_callback) vblank_callback();
}

const uint16_t* gpu_get_vram(void) {
    return vram;
}

static uint8_t gpu_vram_byte(uint32_t byte_x, uint32_t y) {
    uint16_t hw = vram[((y & 511u) * 1024u) + ((byte_x >> 1) & 1023u)];
    return (byte_x & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

/* ---- Present-time screen-colour LUT (verified-enhancement, opt-in) -------
 *
 * PRESENT-TIME ONLY. This sits on the 15-bit-scanout -> RGB888 conversion that
 * feeds the SDL/GL present path. It never touches VRAM and never runs on the
 * depth24 (FMV) scanout (see gpu_display_pixel_rgb). It defaults to SCREEN_RAW
 * (the original exact 5->3-replicated expansion below), so with the feature off
 * the conversion — and therefore every hashed/oracle-diffed frame — is
 * byte-identical to upstream. Opt in via PSX_SCREEN={crt,composite,trinitron};
 * any other value (or unset) keeps the raw path.
 *
 * Built lazily on first use because env is read once and the GPU has no
 * settings plumb yet. The raw fast-path below is preserved verbatim so the
 * default never even consults the LUT. */
static ColorLut* s_screen_lut = NULL;
static int       s_screen_lut_init = 0;
static int       s_screen_kind_cfg = SCREEN_RAW;  /* config/launcher-set; env overrides */

void gpu_set_screen_kind(int kind) {
    if (kind < SCREEN_RAW || kind > SCREEN_TRINITRON) kind = SCREEN_RAW;
    if (kind == s_screen_kind_cfg) return;
    s_screen_kind_cfg = kind;
    s_screen_lut_init = 0;  /* rebuild on next scanout */
}

static void screen_lut_ensure(void) {
    if (s_screen_lut_init) return;
    s_screen_lut_init = 1;
    if (s_screen_lut) { color_lut_destroy(s_screen_lut); s_screen_lut = NULL; }
    /* Precedence: PSX_SCREEN env (debug override) wins if set+valid; otherwise
     * the config/launcher value. Default (no env, raw config) = passthrough. */
    ScreenKind kind = (ScreenKind)s_screen_kind_cfg;
    const char* name = getenv("PSX_SCREEN");
    ScreenKind envk;
    if (name && screen_kind_from_name(name, &envk)) kind = envk;
    if (kind == SCREEN_RAW) {
        s_screen_lut = NULL;  /* raw fast-path; passthrough */
        return;
    }
    ColorSettings s;
    s.screen = kind;
    s.darken = -1.0;          /* per-screen default */
    s.target = DISPLAY_SRGB;
    s_screen_lut = color_lut_create(&s);
    if (s_screen_lut && color_lut_is_passthrough(s_screen_lut)) {
        color_lut_destroy(s_screen_lut);
        s_screen_lut = NULL;
    }
}

static void gpu_rgb555_to_rgb888(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
    screen_lut_ensure();
    if (s_screen_lut) {
        color_lut_map555(s_screen_lut, c, r, g, b);
        return;
    }
    /* Default raw path — byte-identical to upstream. */
    *r = (uint8_t)((c & 0x1Fu) << 3);
    *g = (uint8_t)(((c >> 5) & 0x1Fu) << 3);
    *b = (uint8_t)(((c >> 10) & 0x1Fu) << 3);
}

void gpu_display_pixel_rgb(const GpuDisplayInfo* di, uint32_t x, uint32_t y,
                           uint8_t* r, uint8_t* g, uint8_t* b) {
    if (di->depth24) {
        uint32_t byte_x = ((di->display_x & 1023u) * 2u) + x * 3u;
        uint32_t vy = (di->display_y + y) & 511u;
        *r = gpu_vram_byte(byte_x + 0u, vy);
        *g = gpu_vram_byte(byte_x + 1u, vy);
        *b = gpu_vram_byte(byte_x + 2u, vy);
        return;
    }

    uint32_t vx = (di->display_x + x) & 1023u;
    uint32_t vy = (di->display_y + y) & 511u;
    gpu_rgb555_to_rgb888(vram[vy * 1024u + vx], r, g, b);
}

uint32_t gpu_display_pixel_argb(const GpuDisplayInfo* di, uint32_t x, uint32_t y) {
    uint8_t r, g, b;
    gpu_display_pixel_rgb(di, x, y, &r, &g, &b);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void gpu_get_display_info(GpuDisplayInfo* out) {
    out->display_x = display_area_x;
    out->display_y = display_area_y;
    out->depth24   = (int)(display_depth & 1u);
    out->disabled  = (int)display_disabled;

    /* Derive pixel width from horizontal display range and resolution mode.
     * The BIOS typically sets 256x240 or 320x240 NTSC. */
    uint32_t dots = (h_display_x2 > h_display_x1) ? (h_display_x2 - h_display_x1) : 0;

    /* Horizontal resolution in pixels depends on hres1/hres2 bits.
     * Common: hres1=0 → 256, hres1=1 → 320, hres1=2 → 512, hres1=3 → 640
     *         hres2=1 → 368 */
    uint32_t w;
    if (hres2) {
        w = 368;
    } else {
        static const uint32_t hres_lut[4] = { 256, 320, 512, 640 };
        w = hres_lut[hres1 & 3];
    }

    uint32_t h = (v_display_y2 > v_display_y1) ? (v_display_y2 - v_display_y1) : 240;
    if (vres) h *= 2; /* 480i */

    /* Clamp to sane maximums */
    if (w > 640) w = 640;
    if (h > 512) h = 512;

    out->width  = w;
    out->height = h;
}

void gpu_set_vblank_callback(gpu_vblank_cb cb) {
    vblank_callback = cb;
}

/* Sign-extend an N-bit value to int32_t */
static int32_t sign_extend(uint32_t val, int bits) {
    uint32_t sign_bit = 1u << (bits - 1);
    return (int32_t)((val ^ sign_bit) - sign_bit);
}

/* ---- Polygon rasterizer ---- */

/* Parse a vertex position word: signed 11-bit X and Y */
static void parse_vertex(uint32_t word, int32_t* x, int32_t* y) {
    *x = sign_extend(word & 0x7FFu, 11);
    *y = sign_extend((word >> 16) & 0x7FFu, 11);
}

/* Write a single pixel to VRAM with draw area clipping and mask bit handling */
static void raster_pixel(int32_t x, int32_t y, uint16_t color) {
    if (x < (int32_t)draw_area_left || x > (int32_t)draw_area_right) return;
    if (y < (int32_t)draw_area_top  || y > (int32_t)draw_area_bottom) return;
    uint32_t vx = (uint32_t)x & 1023u;
    uint32_t vy = (uint32_t)y & 511u;
    uint32_t idx = vy * 1024 + vx;
    if (check_mask_bit && (vram[idx] & 0x8000u)) return;
    vram[idx] = color | (set_mask_bit ? 0x8000u : 0u);
}

/* Rasterize a flat-shaded triangle using DDA scanline fill.
 * Vertices are in screen coordinates (draw offset already applied). */
static void raster_triangle(int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1,
                            int32_t x2, int32_t y2,
                            uint16_t color)
{
    /* Sort vertices by Y (ascending). */
    int32_t tx, ty;
    if (y0 > y1) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }
    if (y1 > y2) { tx=x1; ty=y1; x1=x2; y1=y2; x2=tx; y2=ty; }
    if (y0 > y1) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }

    /* Reject degenerate (zero-height) or oversized triangles. */
    if (y0 == y2) return;
    if ((x2 - x0) > 1023 || (x0 - x2) > 1023) return;
    if ((y2 - y0) > 511) return;

    /* 64-bit fixed-point DDA (32.32) matching PS1 hardware behavior.
     * Reference: DuckStation gpu_sw_rasterizer.inl makefp_xy / makestep_xy */
    #define FP_ONE  (1LL << 32)
    #define MAKE_FP(v) (((int64_t)(v) << 32) + (FP_ONE - (1 << 11)))
    #define MAKE_STEP(dx, dy) \
        ((((int64_t)(dx) << 32) + ((dx) < 0 ? -((dy)-1) : (((dx) > 0) ? ((dy)-1) : 0))) / (dy))
    #define UNFP(fp) ((int32_t)((uint64_t)(fp) >> 32))

    /* Upper half: y0 to y1 */
    if (y1 > y0) {
        int32_t dy_long  = y2 - y0;
        int32_t dy_short = y1 - y0;
        int64_t base   = MAKE_FP(x0);
        int64_t step_long  = MAKE_STEP(x2 - x0, dy_long);
        int64_t step_short = MAKE_STEP(x1 - x0, dy_short);
        /* Determine which edge is left vs right */
        int64_t lx, rx, ls, rs;
        if (step_long < step_short) {
            lx = base; ls = step_long; rx = base; rs = step_short;
        } else {
            lx = base; ls = step_short; rx = base; rs = step_long;
        }
        for (int32_t y = y0; y < y1; y++) {
            int32_t xl = UNFP(lx);
            int32_t xr = UNFP(rx);
            for (int32_t x = xl; x < xr; x++)
                raster_pixel(x, y, color);
            lx += ls; rx += rs;
        }
    }

    /* Lower half: y1 to y2 */
    if (y2 > y1) {
        int32_t dy_long  = y2 - y0;
        int32_t dy_short = y2 - y1;
        /* Long edge continues from y0 to y2 */
        int64_t step_long  = MAKE_STEP(x2 - x0, dy_long);
        int64_t long_at_y1 = MAKE_FP(x0) + step_long * (y1 - y0);
        int64_t short_start = MAKE_FP(x1);
        int64_t step_short = MAKE_STEP(x2 - x1, dy_short);
        /* Determine left/right from the upper half's orientation */
        int64_t lx, rx, ls, rs;
        int64_t full_step_short_upper = (y1 > y0) ? MAKE_STEP(x1 - x0, y1 - y0) : 0;
        if (step_long < full_step_short_upper ||
            (y1 == y0 && long_at_y1 <= short_start)) {
            lx = long_at_y1; ls = step_long; rx = short_start; rs = step_short;
        } else {
            lx = short_start; ls = step_short; rx = long_at_y1; rs = step_long;
        }
        for (int32_t y = y1; y < y2; y++) {
            int32_t xl = UNFP(lx);
            int32_t xr = UNFP(rx);
            for (int32_t x = xl; x < xr; x++)
                raster_pixel(x, y, color);
            lx += ls; rx += rs;
        }
    }

    #undef FP_ONE
    #undef MAKE_FP
    #undef MAKE_STEP
    #undef UNFP
}

/* Execute mono triangle (GP0 0x20-0x23) */
static void gp0_exec_mono_tri(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t vx[3], vy[3];
    for (int i = 0; i < 3; i++) {
        parse_vertex(gp0_cmd_buf[1 + i], &vx[i], &vy[i]);
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_triangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], color);
}

/* Execute mono quad (GP0 0x28-0x2B) — two triangles: (0,1,2) and (2,1,3) */
static void gp0_exec_mono_quad(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t vx[4], vy[4];
    for (int i = 0; i < 4; i++) {
        parse_vertex(gp0_cmd_buf[1 + i], &vx[i], &vy[i]);
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_triangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], color);
    gr_draw_flat_triangle(vx[2], vy[2], vx[1], vy[1], vx[3], vy[3], color);
}

/* Execute shaded triangle (GP0 0x30-0x33) — Gouraud shaded */
static void gp0_exec_shaded_tri(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t vx[3], vy[3];
    uint16_t c[3];
    /* Layout: C0, V0, C1, V1, C2, V2 */
    for (int i = 0; i < 3; i++) {
        c[i] = rgb888_to_rgb555(gp0_cmd_buf[i * 2] & 0xFFFFFFu);
        parse_vertex(gp0_cmd_buf[1 + i * 2], &vx[i], &vy[i]);
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_gouraud_triangle(vx[0], vy[0], c[0],
                             vx[1], vy[1], c[1],
                             vx[2], vy[2], c[2]);
}

void gpu_arm_shaded_quad_capture(void) { sq_cap_armed = 1; sq_cap_count = 0; }
int  gpu_get_shaded_quad_capture(const GpuSqCapEntry** out) {
    sq_cap_armed = 0; /* disarm on read */
    *out = sq_cap_buf;
    return sq_cap_count;
}

/* Execute shaded quad (GP0 0x38-0x3B) */
static void gp0_exec_shaded_quad(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t vx[4], vy[4];
    uint16_t c[4];
    /* Layout: C0, V0, C1, V1, C2, V2, C3, V3 */
    for (int i = 0; i < 4; i++) {
        c[i] = rgb888_to_rgb555(gp0_cmd_buf[i * 2] & 0xFFFFFFu);
        parse_vertex(gp0_cmd_buf[1 + i * 2], &vx[i], &vy[i]);
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    /* Capture vertex data when armed. */
    if (sq_cap_armed && sq_cap_count < SQ_CAP_MAX) {
        GpuSqCapEntry* e = &sq_cap_buf[sq_cap_count++];
        for (int i = 0; i < 4; i++) {
            e->vx[i] = vx[i]; e->vy[i] = vy[i];
            e->color[i] = gp0_cmd_buf[i * 2] & 0xFFFFFFu;
        }
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_gouraud_triangle(vx[0], vy[0], c[0],
                             vx[1], vy[1], c[1],
                             vx[2], vy[2], c[2]);
    gr_draw_gouraud_triangle(vx[2], vy[2], c[2],
                             vx[1], vy[1], c[1],
                             vx[3], vy[3], c[3]);
}

/* Helper: build texpage word from GPU state for SW renderer.
 * Format: bits 0-3 = X base, bit 4 = Y base, bits 5-6 = semi-trans, bits 7-8 = color depth */
static uint16_t current_texpage(void) {
    return (uint16_t)(texpage_x | (texpage_y << 4) |
                      (semi_transparency << 5) | (texpage_colors << 7));
}

/* Helper: set up SW renderer state before a textured draw.
 * semi_trans: whether the primitive has semi-transparency enabled (from opcode bit)
 * raw_texture: 1 if this is a raw-texture opcode (bit 0 of GP0 opcode set) */
static void setup_textured_draw(uint32_t color24, int semi_trans, int raw_texture) {
    uint32_t r = (color24 >> 0) & 0xFF;
    uint32_t g = (color24 >> 8) & 0xFF;
    uint32_t b = (color24 >> 16) & 0xFF;
    gr_set_color_modulation((int)r, (int)g, (int)b, raw_texture);
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
}

/* Execute textured triangle (GP0 0x24-0x27) */
static void gp0_exec_textured_tri(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t vx[3], vy[3];
    int u[3], v[3];
    /* Layout: color+v0, texcoord0+clut, v1, texcoord1+tpage, v2, texcoord2 */
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[3], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[5], &vx[2], &vy[2]);
    u[0] = gp0_cmd_buf[2] & 0xFF;        v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[4] & 0xFF;        v[1] = (gp0_cmd_buf[4] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[6] & 0xFF;        v[2] = (gp0_cmd_buf[6] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    /* Texpage from word 4 bits 16-31, or use current state */
    uint16_t tpage_word = (uint16_t)(gp0_cmd_buf[4] >> 16);
    uint16_t tpage = tpage_word & 0x1FF;

    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    setup_textured_draw(color24, semi_trans, raw_texture);
    gr_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                              vx[1], vy[1], u[1], v[1],
                              vx[2], vy[2], u[2], v[2],
                              clut_x, clut_y, tpage);
}

/* Execute textured quad (GP0 0x2C-0x2F) */
static void gp0_exec_textured_quad(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t vx[4], vy[4];
    int u[4], v[4];
    /* Layout: color+v0, tc0+clut, v1, tc1+tpage, v2, tc2, v3, tc3 */
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[3], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[5], &vx[2], &vy[2]);
    parse_vertex(gp0_cmd_buf[7], &vx[3], &vy[3]);
    u[0] = gp0_cmd_buf[2] & 0xFF;  v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[4] & 0xFF;  v[1] = (gp0_cmd_buf[4] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[6] & 0xFF;  v[2] = (gp0_cmd_buf[6] >> 8) & 0xFF;
    u[3] = gp0_cmd_buf[8] & 0xFF;  v[3] = (gp0_cmd_buf[8] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    uint16_t tpage = (uint16_t)(gp0_cmd_buf[4] >> 16) & 0x1FF;

    /* Widescreen: tagged billboard quads carry CPU-computed pixel offsets the
     * GTE squash never saw — re-squash every X around the prim's anchor. */
    {
        int32_t ws_ax;
        if (ws_tagged_anchor(&ws_ax))
            for (int i = 0; i < 4; i++) vx[i] = ws_scale_about(vx[i], ws_ax);
    }

    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    setup_textured_draw(color24, semi_trans, raw_texture);

    if (vy[0] == vy[1] && vy[2] == vy[3] &&
        vx[0] == vx[2] && vx[1] == vx[3] &&
        u[0] == u[2] && u[1] == u[3] &&
        v[0] == v[1] && v[2] == v[3]) {
        int x = vx[0] < vx[1] ? vx[0] : vx[1];
        int y = vy[0] < vy[2] ? vy[0] : vy[2];
        int w = vx[0] < vx[1] ? vx[1] - vx[0] : vx[0] - vx[1];
        int h = vy[0] < vy[2] ? vy[2] - vy[0] : vy[0] - vy[2];
        int left_u  = vx[0] < vx[1] ? u[0] : u[1];
        int right_u = vx[0] < vx[1] ? u[1] : u[0];
        int top_v   = vy[0] < vy[2] ? v[0] : v[2];
        int bot_v   = vy[0] < vy[2] ? v[2] : v[0];
        if (w > 0 && h > 0) {
            if (right_u - left_u == w && bot_v - top_v == h) {
                gr_draw_textured_rect(x, y, w, h, left_u, top_v,
                                      clut_x, clut_y, tpage);
                return;
            }
            gr_draw_textured_rect_scaled(x, y, w, h, left_u, top_v,
                                         right_u, bot_v,
                                         clut_x, clut_y, tpage);
            return;
        }
    }

    gr_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                              vx[1], vy[1], u[1], v[1],
                              vx[2], vy[2], u[2], v[2],
                              clut_x, clut_y, tpage);
    gr_draw_textured_triangle(vx[2], vy[2], u[2], v[2],
                              vx[1], vy[1], u[1], v[1],
                              vx[3], vy[3], u[3], v[3],
                              clut_x, clut_y, tpage);
}

/* Execute shaded textured triangle (GP0 0x34-0x37) */
static void gp0_exec_shaded_textured_tri(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t vx[3], vy[3];
    int u[3], v[3];
    uint32_t c[3];
    /* Layout: C0, V0, TC0+clut, C1, V1, TC1+tpage, C2, V2, TC2 */
    c[0] = gp0_cmd_buf[0] & 0xFFFFFFu;
    c[1] = gp0_cmd_buf[3] & 0xFFFFFFu;
    c[2] = gp0_cmd_buf[6] & 0xFFFFFFu;
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[4], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[7], &vx[2], &vy[2]);
    u[0] = gp0_cmd_buf[2] & 0xFF;  v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[5] & 0xFF;  v[1] = (gp0_cmd_buf[5] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[8] & 0xFF;  v[2] = (gp0_cmd_buf[8] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    uint16_t tpage = (uint16_t)(gp0_cmd_buf[5] >> 16) & 0x1FF;

    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_shaded_textured_triangle(vx[0], vy[0], u[0], v[0], c[0],
                                     vx[1], vy[1], u[1], v[1], c[1],
                                     vx[2], vy[2], u[2], v[2], c[2],
                                     clut_x, clut_y, tpage, raw_texture);
}

/* Execute shaded textured quad (GP0 0x3C-0x3F) */
static void gp0_exec_shaded_textured_quad(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t vx[4], vy[4];
    int u[4], v[4];
    uint32_t c[4];
    /* Layout: C0, V0, TC0+clut, C1, V1, TC1+tpage, C2, V2, TC2, C3, V3, TC3 */
    c[0] = gp0_cmd_buf[0] & 0xFFFFFFu;
    c[1] = gp0_cmd_buf[3] & 0xFFFFFFu;
    c[2] = gp0_cmd_buf[6] & 0xFFFFFFu;
    c[3] = gp0_cmd_buf[9] & 0xFFFFFFu;
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[4], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[7], &vx[2], &vy[2]);
    parse_vertex(gp0_cmd_buf[10], &vx[3], &vy[3]);
    u[0] = gp0_cmd_buf[2] & 0xFF;   v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[5] & 0xFF;   v[1] = (gp0_cmd_buf[5] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[8] & 0xFF;   v[2] = (gp0_cmd_buf[8] >> 8) & 0xFF;
    u[3] = gp0_cmd_buf[11] & 0xFF;  v[3] = (gp0_cmd_buf[11] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    uint16_t tpage = (uint16_t)(gp0_cmd_buf[5] >> 16) & 0x1FF;

    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_shaded_textured_triangle(vx[0], vy[0], u[0], v[0], c[0],
                                     vx[1], vy[1], u[1], v[1], c[1],
                                     vx[2], vy[2], u[2], v[2], c[2],
                                     clut_x, clut_y, tpage, raw_texture);
    gr_draw_shaded_textured_triangle(vx[2], vy[2], u[2], v[2], c[2],
                                     vx[1], vy[1], u[1], v[1], c[1],
                                     vx[3], vy[3], u[3], v[3], c[3],
                                     clut_x, clut_y, tpage, raw_texture);
}

/* Execute mono line (GP0 0x40-0x47) — Bresenham */
static void gp0_exec_mono_line(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0, x1, y1;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    parse_vertex(gp0_cmd_buf[2], &x1, &y1);
    x0 += draw_offset_x; y0 += draw_offset_y;
    x1 += draw_offset_x; y1 += draw_offset_y;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_line(x0, y0, x1, y1, color);
}

/* Execute shaded line (GP0 0x50-0x57) */
static void gp0_exec_shaded_line(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t c0 = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    uint16_t c1 = rgb888_to_rgb555(gp0_cmd_buf[2] & 0xFFFFFFu);
    int32_t x0, y0, x1, y1;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    parse_vertex(gp0_cmd_buf[3], &x1, &y1);
    x0 += draw_offset_x; y0 += draw_offset_y;
    x1 += draw_offset_x; y1 += draw_offset_y;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_shaded_line(x0, y0, c0, x1, y1, c1);
}

/* Execute mono rectangle (GP0 0x60-0x63) */
static void gp0_exec_mono_rect(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int w = gp0_cmd_buf[2] & 0xFFFFu;
    int h = (gp0_cmd_buf[2] >> 16) & 0xFFFFu;
    if (w > 1023) w = 1023;
    if (h > 511)  h = 511;
    x0 += draw_offset_x; y0 += draw_offset_y;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_rect(x0, y0, w, h, color);
}

/* Execute textured rectangle (GP0 0x64-0x67) */
static void gp0_exec_textured_rect(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    int w = gp0_cmd_buf[3] & 0x3FF;
    int h = (gp0_cmd_buf[3] >> 16) & 0x1FF;
    if (w > 1023) w = 1023;
    if (h > 511)  h = 511;

    /* Widescreen: tagged sprite parts squash around their projected anchor;
     * untagged SPRTs are screen-space 2D (HUD/menus) and squash around the
     * display centre. Texels keep full coverage via the scaled-rect path. */
    int ws_w = 0;
    if (ws_active() && w > 0) {
        int32_t ws_ax;
        if (ws_tagged_anchor(&ws_ax)) {
            x0 = ws_scale_about(x0, ws_ax);
            ws_w = (int)ws_scale_len(w);
        } else if (ws_hud_sprt) {
            x0 = ws_scale_about(x0, ws_hud_pivot(x0, w));
            ws_w = (int)ws_scale_len(w);
        }
    }

    x0 += draw_offset_x; y0 += draw_offset_y;
    setup_textured_draw(color24, semi_trans, raw_texture);
    if (ws_w && ws_w != w)
        gr_draw_textured_rect_scaled(x0, y0, ws_w, h, u0, v0, u0 + w, v0 + h,
                                     clut_x, clut_y, current_texpage());
    else
        gr_draw_textured_rect(x0, y0, w, h, u0, v0, clut_x, clut_y, current_texpage());
}

/* Execute 1x1 dot (GP0 0x68-0x6B) */
static void gp0_exec_mono_dot(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x, y;
    parse_vertex(gp0_cmd_buf[1], &x, &y);
    x += draw_offset_x; y += draw_offset_y;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_rect(x, y, 1, 1, color);
}

/* Execute 8x8 textured sprite (GP0 0x74-0x77) */
static void gp0_exec_textured_8x8(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int ws_w = ws_sprt_fixed_transform(&x0, 8);
    x0 += draw_offset_x; y0 += draw_offset_y;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;

    setup_textured_draw(color24, semi_trans, raw_texture);
    if (ws_w && ws_w != 8)
        gr_draw_textured_rect_scaled(x0, y0, ws_w, 8, u0, v0, u0 + 8, v0 + 8,
                                     clut_x, clut_y, current_texpage());
    else
        gr_draw_textured_rect(x0, y0, 8, 8, u0, v0, clut_x, clut_y, current_texpage());
}

/* Execute 8x8 sprite (GP0 0x70-0x73) */
static void gp0_exec_mono_8x8(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    x0 += draw_offset_x; y0 += draw_offset_y;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_rect(x0, y0, 8, 8, color);
}

/* Execute 16x16 textured sprite (GP0 0x7C-0x7F) */
static void gp0_exec_textured_16x16(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int ws_w = ws_sprt_fixed_transform(&x0, 16);
    x0 += draw_offset_x; y0 += draw_offset_y;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;

    setup_textured_draw(color24, semi_trans, raw_texture);
    if (ws_w && ws_w != 16)
        gr_draw_textured_rect_scaled(x0, y0, ws_w, 16, u0, v0, u0 + 16, v0 + 16,
                                     clut_x, clut_y, current_texpage());
    else
        gr_draw_textured_rect(x0, y0, 16, 16, u0, v0, clut_x, clut_y, current_texpage());
}

/* ---- GP0 command execution ---- */

static void gp0_exec_nop(void) {
    /* 0x00 / 0x01 — nothing to do */
}

static void gp0_exec_fill_rect(void) {
    /* 0x02 — Fill Rectangle in VRAM
     * Word 0: 0x02BBGGRR (color)
     * Word 1: YstartXstart (X bits 0-9 aligned to 16, Y bits 16-24)
     * Word 2: YsizXsiz (W bits 0-9 rounded up to 16, H bits 16-24) */
    uint32_t color24 = gp0_cmd_buf[0] & 0x00FFFFFF;
    uint16_t color16 = rgb888_to_rgb555(color24);

    uint32_t dst_x = gp0_cmd_buf[1] & 0x3F0u;          /* X masked to 16-pixel alignment */
    uint32_t dst_y = (gp0_cmd_buf[1] >> 16) & 0x1FFu;
    uint32_t width = ((gp0_cmd_buf[2] & 0x3FFu) + 0xFu) & ~0xFu;  /* round up to 16 */
    uint32_t height = (gp0_cmd_buf[2] >> 16) & 0x1FFu;

    /* Fill ignores draw area, mask bits, and draw offset — writes directly to
     * VRAM. Routed through the renderer so it also fills the hi-res
     * supersampling mirror (no-op cost when supersampling is off). */
    gr_fill_rect((int)dst_x, (int)dst_y, (int)width, (int)height, color16);

    /* Native-wide: when the game clears a display buffer, clear the full width
     * of that buffer's wide surface over the same rows — refreshing the centred
     * content region and keeping the revealed margins clean. */
    if (ws_native_wide_active() && ws_is_fb_base(dst_x))
        gr_wide_clear((int)dst_x, (int)dst_y, (int)height, color16);
}

static void gp0_exec_draw_mode(void) {
    /* 0xE1 — Draw Mode / Texpage
     * Bits 0-3: texpage X base
     * Bit 4: texpage Y base
     * Bits 5-6: semi-transparency mode
     * Bits 7-8: texture color mode
     * Bit 9: dither
     * Bit 10: draw to display area
     * Bit 11: texture disable (when allowed by GP1) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    texpage_x         = param & 0xF;
    texpage_y         = (param >> 4) & 1;
    semi_transparency = (param >> 5) & 3;
    texpage_colors    = (param >> 7) & 3;
    dither_enabled    = (param >> 9) & 1;
    draw_to_display   = (param >> 10) & 1;
    texture_disable   = (param >> 11) & 1;
    /* Sync semi-transparency to SW renderer — actual blending is per-primitive */
}

static void gp0_exec_texture_window(void) {
    /* 0xE2 — Texture Window
     * Bits 0-4: mask X (in 8-pixel steps)
     * Bits 5-9: mask Y
     * Bits 10-14: offset X
     * Bits 15-19: offset Y */
    texture_window_value = gp0_cmd_buf[0] & 0x000FFFFFu;
    gr_set_texture_window(texture_window_value);
}

static void gp0_exec_draw_area_tl(void) {
    /* 0xE3 — Set Drawing Area Top-Left
     * Bits 0-9: X
     * Bits 10-19: Y */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_area_left = param & 0x3FF;
    draw_area_top  = (param >> 10) & 0x3FF;
    gr_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
    ws_nw_sync_target();  /* back buffer (draw_area_left) → wide mirror surface */
}

static void gp0_exec_draw_area_br(void) {
    /* 0xE4 — Set Drawing Area Bottom-Right
     * Bits 0-9: X
     * Bits 10-19: Y */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_area_right  = param & 0x3FF;
    draw_area_bottom = (param >> 10) & 0x3FF;
    gr_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
    ws_nw_sync_target();  /* back buffer (draw_area_left) → wide mirror surface */
}

static void gp0_exec_draw_offset(void) {
    /* 0xE5 — Set Drawing Offset
     * Bits 0-10: X (signed 11-bit)
     * Bits 11-21: Y (signed 11-bit) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_offset_x = sign_extend(param & 0x7FFu, 11);
    draw_offset_y = sign_extend((param >> 11) & 0x7FFu, 11);
    /* Native-wide mirrors framebuffer draws into a separate wide surface (canonical
     * VRAM stays faithful); the renderer needs the live draw offset to translate
     * into surface-local coords, so keep gr_set_draw_offset current. */
    gr_set_draw_offset(draw_offset_x, draw_offset_y);
}

static void gp0_exec_mask_bit(void) {
    /* 0xE6 — Mask Bit Setting
     * Bit 0: set mask bit when drawing (force bit 15 of pixels)
     * Bit 1: check mask bit (don't draw to pixels with bit 15 set) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    set_mask_bit   = param & 1;
    check_mask_bit = (param >> 1) & 1;
    gr_set_mask_bits((int)set_mask_bit, (int)check_mask_bit);
}

/* A0 upload history for debug inspection */
#define A0_HISTORY_CAP 128
typedef struct {
    uint16_t x, y, w, h;
    uint32_t first_words[4];
    int word_count;
    uint32_t func_addr;
    uint32_t sp_val;       /* CPU $sp at time of A0 header */
    uint32_t ra_val;       /* CPU $ra register at time of A0 header */
    uint32_t s1_val;       /* CPU $s1 = source data pointer in func_1FC38524 */
    uint32_t stack[10];    /* first 10 words from sp (sp+32=saved $s1, sp+36=saved $ra) */
} A0HistEntry;
static A0HistEntry a0_history[A0_HISTORY_CAP];
static int a0_history_count = 0;
static int a0_capture_slot = -1;  /* currently capturing data words for this slot */

extern uint32_t psx_read_word(uint32_t addr);

int gpu_get_a0_history(int index, int *x, int *y, int *w, int *h,
                       uint32_t *fw0, uint32_t *fw1, int *wcount) {
    if (index < 0 || index >= a0_history_count) return 0;
    *x = a0_history[index].x; *y = a0_history[index].y;
    *w = a0_history[index].w; *h = a0_history[index].h;
    *fw0 = a0_history[index].first_words[0];
    *fw1 = a0_history[index].first_words[1];
    *wcount = a0_history[index].word_count;
    return 1;
}
int gpu_get_a0_count(void) { return a0_history_count; }
int gpu_get_a0_extra(int index, uint32_t *func, uint32_t *sp, uint32_t *ra,
                     uint32_t *s1, uint32_t *stack10) {
    if (index < 0 || index >= a0_history_count) return 0;
    *func = a0_history[index].func_addr;
    *sp = a0_history[index].sp_val;
    *ra = a0_history[index].ra_val;
    *s1 = a0_history[index].s1_val;
    memcpy(stack10, a0_history[index].stack, 10 * sizeof(uint32_t));
    return 1;
}

static void gp0_exec_cpu_to_vram(void) {
    /* 0xA0 — CPU→VRAM Copy (header: 3 words)
     * Word 0: command
     * Word 1: destination coords (X bits 0-9, Y bits 16-24)
     * Word 2: dimensions (W bits 0-9, H bits 16-24)
     * Followed by pixel data words */
    vram_write_x = gp0_cmd_buf[1] & 0x3FFu;
    vram_write_y = (gp0_cmd_buf[1] >> 16) & 0x1FFu;

    uint32_t w = gp0_cmd_buf[2] & 0x3FFu;
    uint32_t h = (gp0_cmd_buf[2] >> 16) & 0x1FFu;
    /* 0 means max dimension */
    vram_write_w = (w == 0) ? 0x400 : (uint16_t)w;
    vram_write_h = (h == 0) ? 0x200 : (uint16_t)h;

    /* Record for debug */
    if (a0_history_count < A0_HISTORY_CAP) {
        int slot = a0_history_count++;
        a0_history[slot].x = vram_write_x;
        a0_history[slot].y = vram_write_y;
        a0_history[slot].w = vram_write_w;
        a0_history[slot].h = vram_write_h;
        a0_history[slot].word_count = 0;
        a0_history[slot].func_addr = g_debug_current_func_addr;
        memset(a0_history[slot].first_words, 0, sizeof(a0_history[slot].first_words));
        /* Capture CPU context for caller tracing.
         * func_1FC38524 (shell LoadImage) uses $s1 as source data pointer. */
        a0_history[slot].sp_val = 0;
        a0_history[slot].ra_val = 0;
        a0_history[slot].s1_val = 0;
        memset(a0_history[slot].stack, 0, sizeof(a0_history[slot].stack));
        if (debug_cpu_ptr) {
            uint32_t sp = debug_cpu_ptr->gpr[29];
            a0_history[slot].sp_val = sp;
            a0_history[slot].ra_val = debug_cpu_ptr->gpr[31];
            a0_history[slot].s1_val = debug_cpu_ptr->gpr[17]; /* $s1 */
            uint32_t sp_phys = sp & 0x1FFFFFu;
            for (int si = 0; si < 10 && sp_phys + (si + 1) * 4 <= 0x200000u; si++)
                a0_history[slot].stack[si] = psx_read_word(sp + si * 4);
        }
        a0_capture_slot = slot;
    }

    vram_write_col = 0;
    vram_write_row = 0;

    uint32_t num_pixels = (uint32_t)vram_write_w * (uint32_t)vram_write_h;
    vram_write_remaining = (num_pixels + 1) / 2;

    if (vram_write_remaining > 0)
        gp0_state = GP0_VRAM_WRITE;
}

/* C0 (VRAM→CPU) history — uses c0_history_fwd declared at top of file */
#define C0_HISTORY_CAP C0_HISTORY_CAP_FWD
#define c0_history c0_history_fwd
#define c0_history_count c0_history_count_fwd
#define c0_capture_slot c0_capture_slot_fwd

int gpu_get_c0_count(void) { return c0_history_count; }
int gpu_get_c0_history(int index, int *x, int *y, int *w, int *h,
                       uint32_t *func, uint32_t *sp, uint32_t *s1,
                       uint32_t *fw0, uint32_t *fw1, int *rcount) {
    if (index < 0 || index >= c0_history_count) return 0;
    *x = c0_history[index].x; *y = c0_history[index].y;
    *w = c0_history[index].w; *h = c0_history[index].h;
    *func = c0_history[index].func_addr;
    *sp = c0_history[index].sp_val;
    *s1 = c0_history[index].s1_val;
    *fw0 = c0_history[index].first_words[0];
    *fw1 = c0_history[index].first_words[1];
    *rcount = c0_history[index].read_count;
    return 1;
}

static void gp0_exec_vram_to_cpu(void) {
    /* 0xC0 — VRAM→CPU Copy (3 words)
     * Word 1: source coords
     * Word 2: dimensions
     * After this, data is read via GPUREAD */
    vram_read_x = gp0_cmd_buf[1] & 0x3FFu;
    vram_read_y = (gp0_cmd_buf[1] >> 16) & 0x1FFu;

    uint32_t w = gp0_cmd_buf[2] & 0x3FFu;
    uint32_t h = (gp0_cmd_buf[2] >> 16) & 0x1FFu;
    vram_read_w = (w == 0) ? 0x400 : (uint16_t)w;
    vram_read_h = (h == 0) ? 0x200 : (uint16_t)h;

    /* Record for debug */
    if (c0_history_count < C0_HISTORY_CAP) {
        int slot = c0_history_count++;
        c0_history[slot].x = vram_read_x;
        c0_history[slot].y = vram_read_y;
        c0_history[slot].w = vram_read_w;
        c0_history[slot].h = vram_read_h;
        c0_history[slot].func_addr = g_debug_current_func_addr;
        c0_history[slot].sp_val = debug_cpu_ptr ? debug_cpu_ptr->gpr[29] : 0;
        c0_history[slot].s1_val = debug_cpu_ptr ? debug_cpu_ptr->gpr[17] : 0;
        memset(c0_history[slot].first_words, 0, sizeof(c0_history[slot].first_words));
        c0_history[slot].read_count = 0;
        c0_capture_slot = slot;
    }

    vram_read_col = 0;
    vram_read_row = 0;
    vram_read_active = 1;
}

/* Determine how many words a GP0 command requires (header only, not counting
 * variable-length data for 0xA0). Returns -1 for polylines (terminated by
 * sentinel). Returns 0 for unknown commands (will be fatal). */
static int gp0_command_word_count(uint8_t opcode) {
    switch (opcode) {
        /* NOP / control */
        case 0x00: return 1;
        case 0x01: return 1;  /* clear cache */
        case 0x02: return 3;  /* fill rect */
        case 0x1F: return 1;  /* IRQ request */

        /* Drawing commands — polygons */
        case 0x20: case 0x21: case 0x22: case 0x23: return 4;  /* mono tri */
        case 0x24: case 0x25: case 0x26: case 0x27: return 7;  /* textured tri */
        case 0x28: case 0x29: case 0x2A: case 0x2B: return 5;  /* mono quad */
        case 0x2C: case 0x2D: case 0x2E: case 0x2F: return 9;  /* textured quad */
        case 0x30: case 0x31: case 0x32: case 0x33: return 6;  /* shaded tri */
        case 0x34: case 0x35: case 0x36: case 0x37: return 9;  /* shaded textured tri */
        case 0x38: case 0x39: case 0x3A: case 0x3B: return 8;  /* shaded quad */
        case 0x3C: case 0x3D: case 0x3E: case 0x3F: return 12; /* shaded textured quad */

        /* Lines */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: return 3;  /* mono line */
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: return -1; /* mono polyline */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: return 4;  /* shaded line */
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: return -1; /* shaded polyline */

        /* Rectangles */
        case 0x60: case 0x61: case 0x62: case 0x63: return 3;  /* variable rect */
        case 0x64: case 0x65: case 0x66: case 0x67: return 4;  /* variable textured rect */
        case 0x68: case 0x69: case 0x6A: case 0x6B: return 2;  /* 1x1 dot */
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: return 3;  /* 1x1 textured */
        case 0x70: case 0x71: case 0x72: case 0x73: return 2;  /* 8x8 rect */
        case 0x74: case 0x75: case 0x76: case 0x77: return 3;  /* 8x8 textured */
        case 0x78: case 0x79: case 0x7A: case 0x7B: return 2;  /* 16x16 rect */
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: return 3;  /* 16x16 textured */

        /* VRAM copy / transfer */
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F: return 4;  /* VRAM→VRAM */

        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: return 3;  /* CPU→VRAM (header) */

        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF: return 3;  /* VRAM→CPU (header) */

        /* Environment */
        case 0xE0: return 1;  /* NOP */
        case 0xE1: return 1;  /* draw mode */
        case 0xE2: return 1;  /* texture window */
        case 0xE3: return 1;  /* draw area TL */
        case 0xE4: return 1;  /* draw area BR */
        case 0xE5: return 1;  /* draw offset */
        case 0xE6: return 1;  /* mask bits */

        default:
            /* 0x03-0x1E, 0xE7-0xEF, 0xFF: NOP (1 word) per DuckStation */
            if ((opcode >= 0x03 && opcode <= 0x1E) ||
                (opcode >= 0xE7 && opcode <= 0xEF) ||
                opcode == 0xFF) {
                return 1;
            }
            return 0;  /* unknown */
    }
}

/* Per-opcode execution counters (exposed via gpu_get_opcode_stats) */
static uint32_t gp0_opcode_count[256];

uint32_t gpu_get_opcode_count(uint8_t op) { return gp0_opcode_count[op]; }

/* ---- Per-frame GP0 command ring (always-on, queried via debug server) ---- */
/* We record every GP0 command (header + up to 6 payload words) with the
 * frame number it was issued in. Per CLAUDE.md ring-buffer rule: capture
 * is continuous and observers query a window of interest later, not arm-
 * then-record. ~34 MB at 1M entries. Polyline / long commands get the
 * first 6 payload words; that's enough for the header + first vertex pair
 * + first uv/color word for diagnosing per-primitive state. */

extern uint64_t s_frame_count;  /* defined in debug_server.c */
extern uint32_t g_debug_last_store_pc;  /* defined in debug_server.c */

#define GP0_RING_CAP        (1u << 20)  /* 1 048 576 entries */
/* GpuGp0RingEntry + GPU_GP0_RING_MAX_WORDS are public types in gpu.h. */

static GpuGp0RingEntry *gp0_ring = NULL;  /* lazy-alloc on first record */
static uint64_t gp0_ring_seq    = 0;      /* total commands recorded */
static uint32_t gp0_ring_head   = 0;      /* next write slot */

static void gp0_ring_record(const uint32_t *words, int n) {
    if (!gp0_ring) {
        gp0_ring = (GpuGp0RingEntry *)calloc(GP0_RING_CAP, sizeof(*gp0_ring));
        if (!gp0_ring) return;  /* OOM — capture disabled, no other effect */
    }
    GpuGp0RingEntry *e = &gp0_ring[gp0_ring_head];
    e->frame   = (uint32_t)s_frame_count;
    e->seq     = (uint32_t)gp0_ring_seq;
    e->src_addr = gp0_cmd_source_addr;
    e->pc      = g_debug_last_store_pc;
    e->opcode  = (uint8_t)((words[0] >> 24) & 0xFF);
    e->n_words = (uint8_t)(n > 255 ? 255 : (n < 0 ? 1 : n));
    e->pad     = 0;
    int copy_n = e->n_words > GPU_GP0_RING_MAX_WORDS ? GPU_GP0_RING_MAX_WORDS : e->n_words;
    for (int i = 0; i < copy_n; i++) e->cmd[i] = words[i];
    for (int i = copy_n; i < GPU_GP0_RING_MAX_WORDS; i++) e->cmd[i] = 0;
    gp0_ring_head = (gp0_ring_head + 1) % GP0_RING_CAP;
    gp0_ring_seq++;
}

/* Public accessors for debug_server.c */
uint64_t gpu_gp0_ring_total(void)    { return gp0_ring_seq; }
uint32_t gpu_gp0_ring_capacity(void) { return GP0_RING_CAP; }
uint32_t gpu_gp0_ring_max_words(void){ return GPU_GP0_RING_MAX_WORDS; }

void gpu_set_gp0_source(uint32_t addr) {
    gp0_next_source_addr = addr;
}

/* Fill `out[0..max_out-1]` with entries from the requested frame; returns
 * count. Walks from oldest in-buffer to newest so iteration order matches
 * draw order within a frame. */
int gpu_gp0_ring_dump_frame(uint32_t frame, GpuGp0RingEntry *out, int max_out) {
    if (!gp0_ring || max_out <= 0) return 0;
    uint32_t avail = (gp0_ring_seq < GP0_RING_CAP)
                   ? (uint32_t)gp0_ring_seq : GP0_RING_CAP;
    uint32_t start = (gp0_ring_seq < GP0_RING_CAP) ? 0 : gp0_ring_head;
    int n_out = 0;
    for (uint32_t i = 0; i < avail && n_out < max_out; i++) {
        uint32_t idx = (start + i) % GP0_RING_CAP;
        if (gp0_ring[idx].frame == frame) {
            out[n_out++] = gp0_ring[idx];
        }
    }
    return n_out;
}

/* Report the frame range currently held in the ring (oldest..newest). */
void gpu_gp0_ring_frame_span(uint32_t *out_oldest, uint32_t *out_newest) {
    if (out_oldest) *out_oldest = 0;
    if (out_newest) *out_newest = 0;
    if (!gp0_ring || gp0_ring_seq == 0) return;
    uint32_t avail = (gp0_ring_seq < GP0_RING_CAP)
                   ? (uint32_t)gp0_ring_seq : GP0_RING_CAP;
    uint32_t start = (gp0_ring_seq < GP0_RING_CAP) ? 0 : gp0_ring_head;
    uint32_t newest_idx = (start + avail - 1) % GP0_RING_CAP;
    if (out_oldest) *out_oldest = gp0_ring[start].frame;
    if (out_newest) *out_newest = gp0_ring[newest_idx].frame;
}

/* ---- Draw census ring (ALWAYS-ON) -----------------------------------------
 * Every drawn primitive (GP0 opcode 0x20-0x7F) records: frame, the prim's DMA
 * source address, the live camera (scratchpad camX/camY), and the prim's first
 * vertex in DRAWING space (pre draw_offset). Purpose: see object spawn/despawn
 * and edge-cull in DATA, not screenshots. When a background object despawns,
 * its prim simply stops appearing in the census at a specific camX while its
 * last recorded screen-x shows how far on-screen it still was — i.e. the
 * effective (4:3-sized) despawn margin, which the 16:9 view exceeds. A prim
 * present in the census but absent on screen would instead indict the renderer
 * gate. Query via TCP `ws_census` → CSV file (large dumps mustn't ride TCP). */
#define WS_CENSUS_CAP (1u << 21)            /* 2,097,152 entries * 24B = 48 MiB */
typedef struct {
    uint32_t frame;
    uint32_t src_addr;
    int16_t  cam_x, cam_y;
    int16_t  x, y;        /* first vertex (screen / pre-draw_offset) */
    int16_t  xmin, xmax;  /* screen-X extent across the prim's position verts */
    int16_t  base_x;      /* back-buffer origin (draw_area_left) at draw time */
    uint8_t  opcode;
    uint8_t  pad;
} WsCensusEntry;
static WsCensusEntry *ws_census = NULL;
static uint64_t       ws_census_seq = 0;
static int            ws_census_on  = 1;     /* always-on; toggle via ws_census */
extern uint16_t       psx_read_half(uint32_t addr);

/* Screen-X extent (raw vertex coords = SX, pre-draw_offset) across a prim's
 * position vertices. Polygons (0x20-0x3F) decoded exactly via the packet stride
 * (pos word = 1 + i*(1 + gouraud + textured)); rects/sprites/lines fall back to
 * the first vertex. Lets the census show how far right each path reaches and
 * whether anything is drawn past the 4:3 edge (native-wide margin diagnosis). */
static void prim_sx_extent(uint8_t op, int32_t *xmin, int32_t *xmax) {
    int32_t lo = 0x7FFF, hi = -0x8000;
    if (op >= 0x20 && op <= 0x3F) {
        int nv     = (op & 0x08) ? 4 : 3;
        int stride = 1 + ((op & 0x10) ? 1 : 0) + ((op & 0x04) ? 1 : 0);
        for (int i = 0; i < nv; i++) {
            int32_t vx, vy;
            parse_vertex(gp0_cmd_buf[1 + i * stride], &vx, &vy);
            if (vx < lo) lo = vx;
            if (vx > hi) hi = vx;
        }
    } else {
        int32_t vx, vy;
        parse_vertex(gp0_cmd_buf[1], &vx, &vy);
        lo = hi = vx;
    }
    *xmin = lo; *xmax = hi;
}

static void ws_census_record(uint8_t opcode, int32_t x, int32_t y) {
    if (!ws_census_on) return;
    if (!ws_census) {
        ws_census = (WsCensusEntry *)calloc(WS_CENSUS_CAP, sizeof(WsCensusEntry));
        if (!ws_census) { ws_census_on = 0; return; }
    }
    int32_t xmn, xmx;
    prim_sx_extent(opcode, &xmn, &xmx);
    WsCensusEntry *e = &ws_census[ws_census_seq & (WS_CENSUS_CAP - 1)];
    e->frame    = (uint32_t)s_frame_count;
    e->src_addr = gp0_cmd_source_addr;
    e->cam_x    = (int16_t)psx_read_half(0x1F800176);
    e->cam_y    = (int16_t)psx_read_half(0x1F800186);
    e->x        = (int16_t)x;
    e->y        = (int16_t)y;
    e->xmin     = (int16_t)xmn;
    e->xmax     = (int16_t)xmx;
    e->base_x   = (int16_t)draw_area_left;
    e->opcode   = opcode;
    ws_census_seq++;
}

/* Dump census rows for frames [f0,f1] to a CSV file. Returns row count. */
int gpu_ws_census_dump(uint32_t f0, uint32_t f1, const char *path) {
    if (!ws_census) return 0;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "frame,src_addr,cam_x,cam_y,x,y,xmin,xmax,base_x,opcode\n");
    uint64_t total = ws_census_seq;
    uint64_t avail = total < WS_CENSUS_CAP ? total : WS_CENSUS_CAP;
    uint64_t start = total - avail;
    int n = 0;
    for (uint64_t s = start; s < total; s++) {
        WsCensusEntry *e = &ws_census[s & (WS_CENSUS_CAP - 1)];
        if (e->frame < f0 || e->frame > f1) continue;
        fprintf(fp, "%u,0x%08X,%d,%d,%d,%d,%d,%d,%d,0x%02X\n",
                e->frame, e->src_addr, e->cam_x, e->cam_y, e->x, e->y,
                e->xmin, e->xmax, e->base_x, e->opcode);
        n++;
    }
    fclose(fp);
    return n;
}

void gpu_ws_census_set(int on) { ws_census_on = on ? 1 : 0; }
uint64_t gpu_ws_census_seq(void) { return ws_census_seq; }

/* Execute a fully-collected GP0 command */
static void gp0_execute_command(void) {
    uint8_t opcode = (gp0_cmd_buf[0] >> 24) & 0xFF;
    gp0_opcode_count[opcode]++;
    gp0_ring_record(gp0_cmd_buf, gp0_words_needed);

    /* Draw-census: capture every drawing primitive's first vertex + camera. */
    if (opcode >= 0x20 && opcode <= 0x7F) {
        int32_t cvx, cvy;
        parse_vertex(gp0_cmd_buf[1], &cvx, &cvy);
        ws_census_record(opcode, cvx, cvy);
    }

    /* Categorize for diagnostics */
    if (opcode <= 0x01) gp0_nop_count++;
    else if (opcode == 0x02) gp0_fill_count++;
    else if (opcode >= 0x20 && opcode <= 0x7F) gp0_draw_count++;
    else if (opcode >= 0x80 && opcode <= 0xDF) gp0_copy_count++;
    else if (opcode >= 0xE1 && opcode <= 0xE6) gp0_env_count++;

    switch (opcode) {
        case 0x00:
        case 0x01:
            gp0_exec_nop();
            break;

        case 0x02:
            gp0_exec_fill_rect();
            break;

        case 0xE1:
            gp0_exec_draw_mode();
            break;

        case 0xE2:
            gp0_exec_texture_window();
            break;

        case 0xE3:
            gp0_exec_draw_area_tl();
            break;

        case 0xE4:
            gp0_exec_draw_area_br();
            break;

        case 0xE5:
            gp0_exec_draw_offset();
            break;

        case 0xE6:
            gp0_exec_mask_bit();
            break;

        /* Drawing commands — polygons */
        case 0x20: case 0x21: case 0x22: case 0x23:
            gp0_exec_mono_tri();
            break;
        case 0x24: case 0x25: case 0x26: case 0x27:
            gp0_exec_textured_tri();
            break;
        case 0x28: case 0x29: case 0x2A: case 0x2B:
            gp0_exec_mono_quad();
            break;
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            gp0_exec_textured_quad();
            break;
        case 0x30: case 0x31: case 0x32: case 0x33:
            gp0_exec_shaded_tri();
            break;
        case 0x34: case 0x35: case 0x36: case 0x37:
            gp0_exec_shaded_textured_tri();
            break;
        case 0x38: case 0x39: case 0x3A: case 0x3B:
            gp0_exec_shaded_quad();
            break;
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            gp0_exec_shaded_textured_quad();
            break;

        /* Lines */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            gp0_exec_mono_line();
            break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            gp0_exec_shaded_line();
            break;

        /* Rectangles */
        case 0x60: case 0x61: case 0x62: case 0x63:
            gp0_exec_mono_rect();
            break;
        case 0x64: case 0x65: case 0x66: case 0x67:
            gp0_exec_textured_rect();
            break;
        case 0x68: case 0x69: case 0x6A: case 0x6B:
            gp0_exec_mono_dot();
            break;
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
            /* 1x1 textured dot: cmd, vertex, texcoord+clut (no size word) */
            uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
            int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
            int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
            int32_t x0, y0;
            parse_vertex(gp0_cmd_buf[1], &x0, &y0);
            (void)ws_sprt_fixed_transform(&x0, 1);  /* position only; 1px stays 1px */
            x0 += draw_offset_x; y0 += draw_offset_y;
            int u0 = gp0_cmd_buf[2] & 0xFF;
            int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
            uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
            uint16_t clut_x = (clut & 0x3F) * 16;
            uint16_t clut_y = (clut >> 6) & 0x1FF;
            setup_textured_draw(color24, semi_trans, raw_texture);
            gr_draw_textured_rect(x0, y0, 1, 1, u0, v0, clut_x, clut_y, current_texpage());
            break;
        }
        case 0x70: case 0x71: case 0x72: case 0x73:
            gp0_exec_mono_8x8();
            break;
        case 0x74: case 0x75: case 0x76: case 0x77:
            gp0_exec_textured_8x8();
            break;
        case 0x78: case 0x79: case 0x7A: case 0x7B: {
            /* 16x16 mono sprite */
            int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
            uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
            int32_t x0, y0;
            parse_vertex(gp0_cmd_buf[1], &x0, &y0);
            x0 += draw_offset_x; y0 += draw_offset_y;
            gr_set_semi_transparency(semi_trans, (int)semi_transparency);
            gr_draw_flat_rect(x0, y0, 16, 16, color);
            break;
        }
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            gp0_exec_textured_16x16();
            break;

        /* VRAM→VRAM copy */
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
            /* Word 1: source coords, Word 2: dest coords, Word 3: dimensions */
            int src_x = gp0_cmd_buf[1] & 0x3FF;
            int src_y = (gp0_cmd_buf[1] >> 16) & 0x1FF;
            int dst_x = gp0_cmd_buf[2] & 0x3FF;
            int dst_y = (gp0_cmd_buf[2] >> 16) & 0x1FF;
            int w = gp0_cmd_buf[3] & 0x3FF;
            int h = (gp0_cmd_buf[3] >> 16) & 0x1FF;
            if (w == 0) w = 0x400;
            if (h == 0) h = 0x200;
            gr_copy_rect(src_x, src_y, dst_x, dst_y, w, h);
            break;
        }

        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            gp0_exec_cpu_to_vram();
            break;

        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            gp0_exec_vram_to_cpu();
            break;

        default:
            /* NOP range: 0x03-0x1E, 0xE0, 0xE7-0xEF, 0xFF */
            if ((opcode >= 0x03 && opcode <= 0x1E) ||
                opcode == 0xE0 ||
                (opcode >= 0xE7 && opcode <= 0xEF) ||
                opcode == 0xFF) {
                break;  /* NOP — silently consume */
            }

            /* Any other command (drawing, VRAM-to-VRAM, etc.) is not yet
             * implemented. Fatal halt so we know exactly what's needed next,
             * with all rings queryable post-mortem. */
            {
                static char reason[96];
                snprintf(reason, sizeof(reason),
                         "GPU GP0 unimplemented command 0x%02X (word 0x%08X)",
                         opcode, gp0_cmd_buf[0]);
                psx_fatal_halt(reason);
            }
    }
}

/* ---- GP0 write (0x1F801810 write) — command state machine ---- */

uint64_t gpu_get_gp0_count(void) { return gp0_write_count; }

void gpu_get_gp0_stats(uint64_t* nop, uint64_t* fill, uint64_t* draw, uint64_t* env, uint64_t* copy) {
    *nop = gp0_nop_count; *fill = gp0_fill_count;
    *draw = gp0_draw_count; *env = gp0_env_count; *copy = gp0_copy_count;
}

void gpu_get_draw_area(GpuDrawArea* out) {
    out->left = draw_area_left;
    out->top = draw_area_top;
    out->right = draw_area_right;
    out->bottom = draw_area_bottom;
    out->offset_x = draw_offset_x;
    out->offset_y = draw_offset_y;
}

uint16_t gpu_vram_peek(int x, int y) {
    if (x < 0 || x >= 1024 || y < 0 || y >= 512) return 0;
    /* Through the facade so the GL backend syncs its FBO down first. */
    return gr_vram_read(x, y);
}

void gpu_write_gp0(uint32_t val) {
    gp0_write_count++;

    /* State: consuming pixel data for CPU→VRAM transfer */
    if (gp0_state == GP0_VRAM_WRITE) {
        /* Capture first few data words for debug */
        if (a0_capture_slot >= 0 && a0_capture_slot < A0_HISTORY_CAP) {
            int wc = a0_history[a0_capture_slot].word_count++;
            if (wc < 4)
                a0_history[a0_capture_slot].first_words[wc] = val;
        }
        /* Each word contains two 16-bit pixels (low halfword first) */
        for (int i = 0; i < 2; i++) {
            uint16_t pixel = (uint16_t)(val >> (i * 16));
            uint16_t wx = (vram_write_x + vram_write_col) % 1024;
            uint16_t wy = (vram_write_y + vram_write_row) % 512;

            /* Respect mask bit settings. The check reads through the facade:
             * under the GL backend GPU-drawn mask bits live in the FBO, not
             * the CPU array (one sync per burst; free when check is off). */
            if (check_mask_bit && (gr_vram_read((int)wx, (int)wy) & 0x8000))
                goto next_pixel;

            if (set_mask_bit)
                pixel |= 0x8000;

            /* Route through the renderer so the hi-res supersampling mirror
             * is kept coherent (writes native VRAM identically when off). */
            gr_vram_write((int)wx, (int)wy, pixel);

        next_pixel:
            if (++vram_write_col == vram_write_w) {
                vram_write_col = 0;
                if (++vram_write_row == vram_write_h) {
                    /* Transfer complete */
                    gp0_state = GP0_IDLE;
                    vram_write_remaining = 0;
                    return;
                }
            }
        }

        if (--vram_write_remaining == 0)
            gp0_state = GP0_IDLE;

        return;
    }

    /* State: mono polyline — each word is a vertex (or terminator) */
    if (gp0_state == GP0_POLYLINE_MONO) {
        if (val & 0xF000F000u) {
            /* Terminator: bit pattern has high bits set in both halfwords */
            gp0_state = GP0_IDLE;
            return;
        }
        int32_t x, y;
        parse_vertex(val, &x, &y);
        x += draw_offset_x; y += draw_offset_y;
        if (polyline_has_prev) {
            gr_draw_line(polyline_prev_x, polyline_prev_y, x, y, polyline_color);
        }
        polyline_prev_x = x; polyline_prev_y = y;
        polyline_has_prev = 1;
        return;
    }

    /* State: shaded polyline — alternating color, vertex words */
    if (gp0_state == GP0_POLYLINE_SHADED) {
        /* Even words (after cmd) are colors, odd words are vertices.
         * Sequence: [cmd+C0] [V0] [C1] [V1] [C2] [V2] ...
         * polyline_has_prev tracks: 0=need V0, 1=need C_next, 2=need V_next */
        if (!polyline_has_prev) {
            /* First vertex */
            int32_t x, y;
            parse_vertex(val, &x, &y);
            x += draw_offset_x; y += draw_offset_y;
            polyline_prev_x = x; polyline_prev_y = y;
            polyline_prev_c = polyline_color;
            polyline_has_prev = 1;
            return;
        }
        if (polyline_has_prev == 1) {
            /* Expecting color word (or terminator) */
            if (val & 0xF000F000u) {
                gp0_state = GP0_IDLE;
                return;
            }
            polyline_color = rgb888_to_rgb555(val & 0xFFFFFFu);
            polyline_has_prev = 2;
            return;
        }
        /* polyline_has_prev == 2: vertex word */
        {
            int32_t x, y;
            parse_vertex(val, &x, &y);
            x += draw_offset_x; y += draw_offset_y;
            gr_draw_shaded_line(polyline_prev_x, polyline_prev_y, polyline_prev_c,
                                x, y, polyline_color);
            polyline_prev_x = x; polyline_prev_y = y;
            polyline_prev_c = polyline_color;
            polyline_has_prev = 1;
        }
        return;
    }

    /* State: collecting words for a multi-word command */
    if (gp0_state == GP0_COLLECTING) {
        gp0_cmd_buf[gp0_words_collected++] = val;
        if (gp0_words_collected >= gp0_words_needed) {
            gp0_state = GP0_IDLE;
            gp0_execute_command();
        }
        return;
    }

    /* State: IDLE — this is the first word of a new command */
    uint8_t opcode = (val >> 24) & 0xFF;
    int word_count = gp0_command_word_count(opcode);

    if (word_count == 0) {
        /* Unknown command — fatal halt with rings queryable post-mortem */
        static char reason[96];
        snprintf(reason, sizeof(reason),
                 "GPU GP0 unknown command 0x%02X (word 0x%08X)", opcode, val);
        psx_fatal_halt(reason);
    }

    if (word_count < 0) {
        /* Variable-length polyline command.
         * Mono  (0x48-0x4F): [cmd+color] [v0] [v1] ... [terminator]
         * Shaded(0x58-0x5F): [cmd+C0] [v0] [C1] [v1] ... [terminator]
         * Terminator: word with bit 31 set (0x5xxx or 0xFFFF). */
        int shaded = (opcode & 0x10) != 0;  /* 0x58+ = shaded, 0x48+ = mono */
        polyline_semi_trans = (val >> 25) & 1;
        polyline_color = rgb888_to_rgb555(val & 0xFFFFFFu);
        polyline_prev_c = polyline_color;
        polyline_has_prev = 0;
        gr_set_semi_transparency(polyline_semi_trans, (int)semi_transparency);
        gp0_state = shaded ? GP0_POLYLINE_SHADED : GP0_POLYLINE_MONO;
        gp0_draw_count++;
        /* Record polyline header (variable-length body not captured;
         * just enough so per-frame stream shows the polyline existed). */
        gp0_opcode_count[opcode]++;
        uint32_t hdr_only[1] = { val };
        gp0_ring_record(hdr_only, 1);
        return;
    }

    gp0_cmd_buf[0] = val;
    gp0_cmd_source_addr = gp0_next_source_addr;

    if (word_count == 1) {
        gp0_words_collected = 1;
        gp0_words_needed = 1;
        gp0_execute_command();
    } else {
        gp0_state = GP0_COLLECTING;
        gp0_words_collected = 1;
        gp0_words_needed = word_count;
    }
}

/* ---- GP1 write (0x1F801814 write) ---- */

static void gp1_reset(void) {
    /* GP1(00h): Reset GPU — clears FIFO, resets all state, display off */
    gpu_init();
}

static void gp1_reset_command_buffer(void) {
    /* GP1(01h): Reset command buffer — clears FIFO, aborts current command */
    gp0_state = GP0_IDLE;
    gp0_words_collected = 0;
    gp0_words_needed = 0;
    vram_write_remaining = 0;
}

static void gp1_ack_irq1(void) {
    /* GP1(02h): Acknowledge IRQ1 */
    irq1_flag = 0;
}

static void gp1_display_enable(uint32_t val) {
    /* GP1(03h): Display enable — bit 0: 0=on, 1=off */
    display_disabled = val & 1;
}

static void gp1_dma_direction(uint32_t val) {
    /* GP1(04h): DMA direction — bits 0-1 */
    dma_direction = val & 3;
}

static void gp1_display_area_start(uint32_t val) {
    /* GP1(05h): Start of display area in VRAM
     * bits 0-9: X (in halfwords, 0-1023)
     * bits 10-18: Y (0-511) */
    display_area_x = val & 0x3FF;
    display_area_y = (val >> 10) & 0x1FF;
    ws_note_display_base(display_area_x);  /* learn the display buffer set (native-wide) */
}

static void gp1_h_display_range(uint32_t val) {
    /* GP1(06h): Horizontal display range
     * bits 0-11: X1
     * bits 12-23: X2 */
    h_display_x1 = val & 0xFFF;
    h_display_x2 = (val >> 12) & 0xFFF;
}

static void gp1_v_display_range(uint32_t val) {
    /* GP1(07h): Vertical display range
     * bits 0-9: Y1
     * bits 10-19: Y2 */
    v_display_y1 = val & 0x3FF;
    v_display_y2 = (val >> 10) & 0x3FF;
}

static void gp1_display_mode(uint32_t val) {
    /* GP1(08h): Display mode
     * bit 0-1: horizontal resolution 1 (0=256, 1=320, 2=512, 3=640)
     * bit 2: vertical resolution (0=240, 1=480)
     * bit 3: video mode (0=NTSC, 1=PAL)
     * bit 4: display area color depth (0=15bit, 1=24bit)
     * bit 5: vertical interlace (0=off, 1=on)
     * bit 6: horizontal resolution 2 (0=normal, 1=368)
     * bit 7: "reverseflag" */
    hres1 = val & 3;
    vres = (val >> 2) & 1;
    video_mode = (val >> 3) & 1;
    display_depth = (val >> 4) & 1;
    vertical_interlace = (val >> 5) & 1;
    hres2 = (val >> 6) & 1;
    reverse_flag = (val >> 7) & 1;
}

static void gp1_get_info(uint32_t val) {
    /* GP1(10h): Get GPU info — writes result to GPUREAD latch.
     * Mednafen-psx masks the subcommand to 4 bits (val & 0x0F) and
     * services cases 2..5, 7, 8. Tomba's ResetGraph() uses param 7 to
     * read the GPU version (must be 2) to pick its video-mode path —
     * the wrong value here lands the game on a no-draw branch. */
    uint32_t which = val & 0x0F;
    switch (which) {
        case 2: /* texture window */
            gpuread_latch = texture_window_value;
            break;
        case 3: /* draw area top-left */
            gpuread_latch = draw_area_left | (draw_area_top << 10);
            break;
        case 4: /* draw area bottom-right */
            gpuread_latch = draw_area_right | (draw_area_bottom << 10);
            break;
        case 5: /* draw offset */
            gpuread_latch = ((uint32_t)draw_offset_x & 0x7FFu) |
                            (((uint32_t)draw_offset_y & 0x7FFu) << 11);
            break;
        case 7: /* GPU version (real-hw + mednafen return 2) */
            gpuread_latch = 2;
            break;
        case 8: /* unknown info index, real hw / mednafen return 0 */
            gpuread_latch = 0;
            break;
        default:
            /* N=0,1,6,9..15: leave latch unchanged */
            break;
    }
}

void gpu_write_gp1(uint32_t val) {
    uint32_t cmd = (val >> 24) & 0x3F;

    switch (cmd) {
        case 0x00: gp1_reset(); break;
        case 0x01: gp1_reset_command_buffer(); break;
        case 0x02: gp1_ack_irq1(); break;
        case 0x03: gp1_display_enable(val); break;
        case 0x04: gp1_dma_direction(val); break;
        case 0x05: gp1_display_area_start(val); break;
        case 0x06: gp1_h_display_range(val); break;
        case 0x07: gp1_v_display_range(val); break;
        case 0x08: gp1_display_mode(val); break;
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
            gp1_get_info(val); break;
        default: {
            static char reason[96];
            snprintf(reason, sizeof(reason),
                     "GPU GP1 unknown command 0x%02X (word 0x%08X)", cmd, val);
            psx_fatal_halt(reason);
        }
    }
}

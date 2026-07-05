#include "gte.h"
#include "cpu_state.h"
#include <algorithm>
#include <cstdlib>

namespace PSXRecomp {
namespace GTE {

// ---------------------------------------------------------------------------
// Common helpers
// ---------------------------------------------------------------------------

// Perspective division: H / SZ3, scaled, saturated to 17 bits.
//
// The real PS1 GTE does NOT compute an exact H*0x20000/SZ3. It uses an
// Unsigned Newton-Raphson (UNR) reciprocal approximation driven by a 257-entry
// seed table (documented in PSX-SPX "GTE Division Inaccuracy"; identical to the
// mednafen/Beetle oracle in beetle-psx/mednafen/psx/gte.cpp). Exact division
// diverges from hardware by +/-1 (occasionally up to a few units) on ~25% of
// inputs, which is enough to flip games' distance/intensity threshold branches
// (e.g. Ape Escape's additive-glow CLUT semi-transparency bit). This is the
// faithful hardware algorithm, shared by every RTPS/RTPT caller.
static uint8_t s_gte_div_table[0x101];
static bool    s_gte_div_table_init = false;
static void gte_init_div_table() {
    for (uint32_t divisor = 0x8000; divisor < 0x10000; divisor += 0x80) {
        uint32_t xa = 512;
        for (unsigned i = 1; i < 5; i++)
            xa = (xa * (1024u * 512u - ((divisor >> 7) * xa))) >> 18;
        s_gte_div_table[(divisor >> 7) & 0xFF] =
            (uint8_t)(((xa + 1) >> 1) - 0x101);
    }
    s_gte_div_table[0x100] = s_gte_div_table[0xFF];
    s_gte_div_table_init = true;
}
static int32_t gte_calc_recip(uint16_t divisor) {
    int32_t x    = 0x101 + s_gte_div_table[(((divisor & 0x7FFF) + 0x40) >> 7)];
    int32_t tmp  = (((int32_t)divisor * -x) + 0x80) >> 8;
    int32_t tmp2 = ((x * (131072 + tmp)) + 0x80) >> 8;
    return tmp2;
}
// count leading zeros of a 16-bit value (0 -> 16)
static inline unsigned gte_clz16(uint16_t v) {
    unsigned n = 0;
    for (int b = 15; b >= 0; --b) { if (v & (1u << b)) break; ++n; }
    return n;
}
static int32_t gte_divide(uint16_t H, uint16_t SZ3, uint32_t& FLAG) {
    if (!s_gte_div_table_init) gte_init_div_table();
    // Hardware: overflow flag + saturate when 2*SZ3 <= H (includes SZ3 == 0).
    if ((uint32_t)SZ3 * 2 <= (uint32_t)H) {
        FLAG |= FLAG_DIV_OVF;
        return 0x1FFFF;
    }
    unsigned shift_bias = gte_clz16(SZ3);
    uint32_t dividend = (uint32_t)H   << shift_bias;
    uint32_t divisor  = (uint32_t)SZ3 << shift_bias;
    uint32_t result = (uint32_t)(((uint64_t)dividend *
                                  (uint32_t)gte_calc_recip((uint16_t)(divisor | 0x8000))
                                  + 32768) >> 16);
    if (result > 0x1FFFF) result = 0x1FFFF;   // 17-bit saturate (no flag; matches hw path)
    return (int32_t)result;
}

// Transform a vertex by Light matrix → IR1/IR2/IR3 (step 1 of lighting)
static void light_transform(GTEState* gte, int16_t* V) {
    int64_t mac1 = ((int64_t)gte->L[0][0] * V[0] +
                    (int64_t)gte->L[0][1] * V[1] +
                    (int64_t)gte->L[0][2] * V[2]) >> 12;
    int64_t mac2 = ((int64_t)gte->L[1][0] * V[0] +
                    (int64_t)gte->L[1][1] * V[1] +
                    (int64_t)gte->L[1][2] * V[2]) >> 12;
    int64_t mac3 = ((int64_t)gte->L[2][0] * V[0] +
                    (int64_t)gte->L[2][1] * V[1] +
                    (int64_t)gte->L[2][2] * V[2]) >> 12;
    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, true);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, true);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, true);
}

// Apply light color matrix + background color to IR → IR (step 2 of lighting)
static void light_color(GTEState* gte) {
    int64_t mac1 = ((int64_t)gte->BK[0] << 12) +
                   (int64_t)gte->LC[0][0] * gte->IR1 +
                   (int64_t)gte->LC[0][1] * gte->IR2 +
                   (int64_t)gte->LC[0][2] * gte->IR3;
    int64_t mac2 = ((int64_t)gte->BK[1] << 12) +
                   (int64_t)gte->LC[1][0] * gte->IR1 +
                   (int64_t)gte->LC[1][1] * gte->IR2 +
                   (int64_t)gte->LC[1][2] * gte->IR3;
    int64_t mac3 = ((int64_t)gte->BK[2] << 12) +
                   (int64_t)gte->LC[2][0] * gte->IR1 +
                   (int64_t)gte->LC[2][1] * gte->IR2 +
                   (int64_t)gte->LC[2][2] * gte->IR3;
    gte->MAC1 = static_cast<int32_t>(mac1 >> 12);
    gte->MAC2 = static_cast<int32_t>(mac2 >> 12);
    gte->MAC3 = static_cast<int32_t>(mac3 >> 12);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, true);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, true);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, true);
}

// Multiply IR by RGBC color and push to RGB FIFO (step 3 of color output)
static void color_output(GTEState* gte) {
    uint8_t r0 = (gte->RGBC >> 0)  & 0xFF;
    uint8_t g0 = (gte->RGBC >> 8)  & 0xFF;
    uint8_t b0 = (gte->RGBC >> 16) & 0xFF;
    int64_t mac1 = ((int64_t)r0 * gte->IR1) << 4;
    int64_t mac2 = ((int64_t)g0 * gte->IR2) << 4;
    int64_t mac3 = ((int64_t)b0 * gte->IR3) << 4;
    gte->MAC1 = static_cast<int32_t>(mac1 >> 12);
    gte->MAC2 = static_cast<int32_t>(mac2 >> 12);
    gte->MAC3 = static_cast<int32_t>(mac3 >> 12);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, true);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, true);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, true);
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
}

// Depth cue from current IR values using IR0 interpolation toward far color
// MAC = FC * 0x1000 - (BK + LC*IR) * 0x1000 ; then IR = (BK+LC*IR) + IR0 * MAC / 0x1000
static void depth_cue_from_ir(GTEState* gte) {
    // Interpolate: result = IR + IR0 * (FC - IR) / 0x1000
    int64_t mac1 = ((int64_t)gte->FC[0] << 12) - ((int64_t)gte->IR1 << 12);
    int64_t mac2 = ((int64_t)gte->FC[1] << 12) - ((int64_t)gte->IR2 << 12);
    int64_t mac3 = ((int64_t)gte->FC[2] << 12) - ((int64_t)gte->IR3 << 12);
    // Scale by IR0 and add back
    mac1 = (int64_t)gte->IR1 + ((gte->IR0 * (mac1 >> 12)) >> 12);
    mac2 = (int64_t)gte->IR2 + ((gte->IR0 * (mac2 >> 12)) >> 12);
    mac3 = (int64_t)gte->IR3 + ((gte->IR0 * (mac3 >> 12)) >> 12);
    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, true);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, true);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, true);
}

// ---------------------------------------------------------------------------
// Widescreen X-squash (verified-enhancement, default off = identity).
//
// For a display aspect wider than the native 4:3, screen-space X is scaled by
// (4*den)/(3*num) around the projection centre OFX — e.g. 3/4 for 16:9 — and
// the present path stretches the 4:3 frame to the wide aspect, netting a wider
// horizontal field of view (the DuckStation/Beetle "widescreen hack", but
// applied in our GTE library so every RTPS/RTPT caller — generated code,
// interpreter, overlay DLLs — sees it). Only the IR1*h/sz term is scaled, NOT
// OFX, so the squash is centred on the game's own projection centre and
// games' post-projection screen-bounds culls (which read SXY back from us)
// stay aligned with the visible frame.
// ---------------------------------------------------------------------------
static int32_t s_ws_xnum = 1, s_ws_xden = 1;
extern "C" int gpu_ws_present_native_43(void);  /* gpu.c — suppress on 4:3 frames */

// Per-draw suppression of the X-squash (8C far-backdrop). The far backdrop
// (ocean/cloud/distant mountain) is a parallax layer that is conceptually at
// infinity: squashing it toward centre leaves its geometry short of the
// revealed 16:9 edges (blue void). Drawing it UN-squashed lets its 4:3-extent
// fill the stretched frame (skybox treatment) while the near 3D world keeps the
// wider FOV. The recompiler brackets the backdrop driver (FUN_8004db3c) with
// gte_ws_set_suppress(1)/(0) ([widescreen] backdrop_unsquash_funcs).
// The driver (FUN_8004db3c) actually draws a MIX: the far backdrop (which we
// want un-squashed) AND nearer props (doors/mansion models) that must stay
// squashed or they drift relative to the rest of the world. So suppression is
// DEPTH-GATED: only vertices with SZ (projected Z) >= s_ws_far_threshold get
// un-squashed while suppress is active; nearer props keep the squash. Threshold
// tuned live via the ws_far_threshold TCP cmd; SZ stats below characterize the
// driver's depth range so the split is set from data, not a guess.
static int s_ws_suppress = 0;
static int32_t s_ws_far_threshold = 900;  /* SZ split: near props squashed, far backdrop un-squashed (8C, tunable via ws_far_threshold) */
extern "C" void gte_ws_set_suppress(int on) { s_ws_suppress = on ? 1 : 0; }
extern "C" void gte_ws_set_far_threshold(int t) { s_ws_far_threshold = t; }
extern "C" int  gte_ws_get_far_threshold(void) { return (int)s_ws_far_threshold; }
/* SZ stats observed while suppress is active (one frame window, reset on read). */
static int32_t s_ws_sz_min = 0x7FFFFFFF, s_ws_sz_max = 0;
static uint32_t s_ws_sz_n = 0, s_ws_sz_far = 0;
extern "C" void gte_ws_get_sz_stats(int* mn, int* mx, unsigned* n, unsigned* far_n) {
    if (mn) *mn = (s_ws_sz_n ? s_ws_sz_min : 0);
    if (mx) *mx = s_ws_sz_max;
    if (n)  *n  = s_ws_sz_n;
    if (far_n) *far_n = s_ws_sz_far;
    s_ws_sz_min = 0x7FFFFFFF; s_ws_sz_max = 0; s_ws_sz_n = 0; s_ws_sz_far = 0;
}

extern "C" void gte_set_display_aspect(int num, int den) {
    if (num <= 0 || den <= 0) { s_ws_xnum = s_ws_xden = 1; return; }
    // squash = (4/3) / (num/den) = (4*den) / (3*num); identity for 4:3.
    int32_t n = 4 * den, d = 3 * num;
    int32_t a = n, b = d;
    while (b) { int32_t t = a % b; a = b; b = t; }   // gcd
    s_ws_xnum = n / a;
    s_ws_xden = d / a;
}

// ---------------------------------------------------------------------------
// RTPS — Perspective Transformation (internal, operates on given vertex V)
// ---------------------------------------------------------------------------
void gte_rtps_internal(GTEState* gte, int16_t* V, bool setMac0) {
    // Step 1: Matrix multiplication + translation
    int64_t mac1 = ((int64_t)gte->TR[0] << 12) +
                   (int64_t)gte->RT[0][0] * V[0] +
                   (int64_t)gte->RT[0][1] * V[1] +
                   (int64_t)gte->RT[0][2] * V[2];
    int64_t mac2 = ((int64_t)gte->TR[1] << 12) +
                   (int64_t)gte->RT[1][0] * V[0] +
                   (int64_t)gte->RT[1][1] * V[1] +
                   (int64_t)gte->RT[1][2] * V[2];
    int64_t mac3 = ((int64_t)gte->TR[2] << 12) +
                   (int64_t)gte->RT[2][0] * V[0] +
                   (int64_t)gte->RT[2][1] * V[1] +
                   (int64_t)gte->RT[2][2] * V[2];

    gte->check_mac_overflow(mac1 >> 12, 1);
    gte->check_mac_overflow(mac2 >> 12, 2);
    gte->check_mac_overflow(mac3 >> 12, 3);
    gte->MAC1 = static_cast<int32_t>(mac1 >> 12);
    gte->MAC2 = static_cast<int32_t>(mac2 >> 12);
    gte->MAC3 = static_cast<int32_t>(mac3 >> 12);

    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, false);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, false);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, false);

    // Step 2: Push SZ FIFO
    gte->push_sz(gte->MAC3);

    // Step 3: Perspective division
    int32_t h_div_sz = gte_divide(gte->H, gte->SZ[3], gte->FLAG);

    // Step 4: Project to screen coordinates. Squash X only when configured AND
    // this frame is being stretched — never on a 4:3-presented frame (FMV /
    // full-2D screen), so content and present stay locked.
    int64_t xterm = (int64_t)gte->IR1 * h_div_sz;
    bool do_squash = (s_ws_xnum != s_ws_xden) && !gpu_ws_present_native_43();
    if (do_squash && s_ws_suppress) {
        /* Depth-gated: un-squash only FAR geometry (the backdrop); keep near
         * props squashed so they stay aligned. Record SZ stats for tuning. */
        int32_t sz = gte->SZ[3];
        if (sz < s_ws_sz_min) s_ws_sz_min = sz;
        if (sz > s_ws_sz_max) s_ws_sz_max = sz;
        s_ws_sz_n++;
        if (sz >= s_ws_far_threshold) { do_squash = false; s_ws_sz_far++; }
    }
    if (do_squash)
        xterm = xterm * s_ws_xnum / s_ws_xden;
    int64_t sx = (gte->OFX + xterm) >> 16;
    int64_t sy = (gte->OFY + (int64_t)gte->IR2 * h_div_sz) >> 16;
    gte->push_sxy(sx, sy);

    // Step 5: Depth cueing (MAC0/IR0) — only for last vertex of RTPT or RTPS
    if (setMac0) {
        int64_t mac0 = (int64_t)gte->DQA * h_div_sz + gte->DQB;
        gte->check_mac0_overflow(mac0);
        gte->MAC0 = static_cast<int32_t>(mac0);
        gte->IR0 = gte->saturate_ir0(static_cast<int32_t>(mac0 >> 12));
    }
}

// RTPS (0x01) — single vertex, always sets MAC0
void gte_rtps(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_rtps_internal(gte, gte->V0, true);
    gte->set_error_flag();
}

// RTPT (0x30) — triple vertex, only last sets MAC0
void gte_rtpt(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_rtps_internal(gte, gte->V0, false);
    gte_rtps_internal(gte, gte->V1, false);
    gte_rtps_internal(gte, gte->V2, true);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCLIP (0x06) — Normal Clipping (2D cross product for backface culling)
// ---------------------------------------------------------------------------
void gte_nclip(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int16_t sx0 = static_cast<int16_t>(gte->SXY[0] & 0xFFFF);
    int16_t sy0 = static_cast<int16_t>(gte->SXY[0] >> 16);
    int16_t sx1 = static_cast<int16_t>(gte->SXY[1] & 0xFFFF);
    int16_t sy1 = static_cast<int16_t>(gte->SXY[1] >> 16);
    int16_t sx2 = static_cast<int16_t>(gte->SXY[2] & 0xFFFF);
    int16_t sy2 = static_cast<int16_t>(gte->SXY[2] >> 16);
    int64_t mac0 = (int64_t)sx0 * (sy1 - sy2) +
                   (int64_t)sx1 * (sy2 - sy0) +
                   (int64_t)sx2 * (sy0 - sy1);
    gte->check_mac0_overflow(mac0);
    gte->MAC0 = static_cast<int32_t>(mac0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// AVSZ3 (0x2D) — Average Z (3 points)
// ---------------------------------------------------------------------------
void gte_avsz3(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int64_t mac0 = (int64_t)gte->ZSF3 * (gte->SZ[1] + gte->SZ[2] + gte->SZ[3]);
    gte->check_mac0_overflow(mac0);
    gte->MAC0 = static_cast<int32_t>(mac0 >> 12);
    gte->OTZ = gte->saturate_sz(gte->MAC0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// AVSZ4 (0x2E) — Average Z (4 points)
// ---------------------------------------------------------------------------
void gte_avsz4(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int64_t mac0 = (int64_t)gte->ZSF4 * (gte->SZ[0] + gte->SZ[1] + gte->SZ[2] + gte->SZ[3]);
    gte->check_mac0_overflow(mac0);
    gte->MAC0 = static_cast<int32_t>(mac0 >> 12);
    gte->OTZ = gte->saturate_sz(gte->MAC0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCCS (0x1B) — Normal Color Color Single
// ---------------------------------------------------------------------------
void gte_nccs_internal(GTEState* gte, int16_t* V) {
    light_transform(gte, V);
    light_color(gte);
    color_output(gte);
}

void gte_nccs(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_nccs_internal(gte, gte->V0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCCT (0x3F) — Normal Color Color Triple
// ---------------------------------------------------------------------------
void gte_ncct(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_nccs_internal(gte, gte->V0);
    gte_nccs_internal(gte, gte->V1);
    gte_nccs_internal(gte, gte->V2);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCDS (0x13) — Normal Color Depth Cue Single
// ---------------------------------------------------------------------------
void gte_ncds_internal(GTEState* gte, int16_t* V) {
    light_transform(gte, V);
    light_color(gte);
    depth_cue_from_ir(gte);
    color_output(gte);
}

void gte_ncds(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_ncds_internal(gte, gte->V0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCDT (0x16) — Normal Color Depth Cue Triple
// ---------------------------------------------------------------------------
void gte_ncdt(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_ncds_internal(gte, gte->V0);
    gte_ncds_internal(gte, gte->V1);
    gte_ncds_internal(gte, gte->V2);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCS (0x1E) — Normal Color Single (no vertex color multiply)
// ---------------------------------------------------------------------------
void gte_ncs_internal(GTEState* gte, int16_t* V) {
    light_transform(gte, V);
    light_color(gte);
    // Output directly (no RGBC multiply)
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
}

void gte_ncs(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_ncs_internal(gte, gte->V0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCT (0x20) — Normal Color Triple
// ---------------------------------------------------------------------------
void gte_nct(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_ncs_internal(gte, gte->V0);
    gte_ncs_internal(gte, gte->V1);
    gte_ncs_internal(gte, gte->V2);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// DPCS (0x10) — Depth Cueing Single (from RGBC color)
// ---------------------------------------------------------------------------
void gte_dpcs(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    uint8_t r = (gte->RGBC >> 0)  & 0xFF;
    uint8_t g = (gte->RGBC >> 8)  & 0xFF;
    uint8_t b = (gte->RGBC >> 16) & 0xFF;
    // MAC = RGBC << 4
    gte->IR1 = gte->saturate_ir(r << 4, 1, false);
    gte->IR2 = gte->saturate_ir(g << 4, 2, false);
    gte->IR3 = gte->saturate_ir(b << 4, 3, false);
    // Interpolate toward far color using IR0
    depth_cue_from_ir(gte);
    // Output
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// DPCT (0x2A) — Depth Cueing Triple (from RGB FIFO entries)
// ---------------------------------------------------------------------------
void gte_dpct(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t r = (gte->RGB[0] >> 0)  & 0xFF;
        uint8_t g = (gte->RGB[0] >> 8)  & 0xFF;
        uint8_t b = (gte->RGB[0] >> 16) & 0xFF;
        gte->IR1 = gte->saturate_ir(r << 4, 1, false);
        gte->IR2 = gte->saturate_ir(g << 4, 2, false);
        gte->IR3 = gte->saturate_ir(b << 4, 3, false);
        depth_cue_from_ir(gte);
        gte->push_rgb(
            gte->saturate_color(gte->MAC1 >> 4, 0),
            gte->saturate_color(gte->MAC2 >> 4, 1),
            gte->saturate_color(gte->MAC3 >> 4, 2));
    }
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// DPCL (0x29) — Depth Cueing Light (from IR, uses existing lighting result)
// ---------------------------------------------------------------------------
void gte_dpcl(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    // IR already contains lighting result, multiply by RGBC
    uint8_t r = (gte->RGBC >> 0)  & 0xFF;
    uint8_t g = (gte->RGBC >> 8)  & 0xFF;
    uint8_t b = (gte->RGBC >> 16) & 0xFF;
    gte->MAC1 = (r * gte->IR1) >> 8;
    gte->MAC2 = (g * gte->IR2) >> 8;
    gte->MAC3 = (b * gte->IR3) >> 8;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, true);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, true);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, true);
    // Depth cue toward far color
    depth_cue_from_ir(gte);
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// INTPL (0x11) — Interpolation (IR toward far color using IR0)
// ---------------------------------------------------------------------------
void gte_intpl(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    depth_cue_from_ir(gte);
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// CDP (0x14) — Color Depth Cue (light color + depth cue, no normal transform)
// ---------------------------------------------------------------------------
void gte_cdp(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    // IR1/IR2/IR3 already set (from previous NCS or similar)
    light_color(gte);
    depth_cue_from_ir(gte);
    color_output(gte);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// CC (0x1C) — Color Color (light color + vertex color, no depth cue)
// ---------------------------------------------------------------------------
void gte_cc(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    light_color(gte);
    color_output(gte);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// MVMVA (0x12) — Matrix Vector Multiply Add
// Highly configurable: matrix, vector, and translation selected by bits
// ---------------------------------------------------------------------------
void gte_mvmva(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    uint32_t mx = (instr >> 17) & 3;  // Matrix: 0=RT, 1=Light, 2=LightColor, 3=reserved
    uint32_t vv = (instr >> 15) & 3;  // Vector: 0=V0, 1=V1, 2=V2, 3=IR
    uint32_t tv = (instr >> 13) & 3;  // Translation: 0=TR, 1=BK, 2=FC/bugged, 3=none
    int sf = (instr >> 19) & 1;        // Shift: 0=no shift, 1=shift right 12

    // Select matrix
    int16_t M[3][3];
    switch (mx) {
        case 0: std::memcpy(M, gte->RT, sizeof(M)); break;
        case 1: std::memcpy(M, gte->L, sizeof(M)); break;
        case 2: std::memcpy(M, gte->LC, sizeof(M)); break;
        default: std::memset(M, 0, sizeof(M)); break; // Garbage on real HW
    }

    // Select vector
    int16_t V[3];
    switch (vv) {
        case 0: V[0] = gte->V0[0]; V[1] = gte->V0[1]; V[2] = gte->V0[2]; break;
        case 1: V[0] = gte->V1[0]; V[1] = gte->V1[1]; V[2] = gte->V1[2]; break;
        case 2: V[0] = gte->V2[0]; V[1] = gte->V2[1]; V[2] = gte->V2[2]; break;
        case 3: V[0] = gte->IR1;   V[1] = gte->IR2;   V[2] = gte->IR3;   break;
    }

    // Select translation vector
    int64_t T[3];
    switch (tv) {
        case 0: T[0] = (int64_t)gte->TR[0] << 12; T[1] = (int64_t)gte->TR[1] << 12; T[2] = (int64_t)gte->TR[2] << 12; break;
        case 1: T[0] = (int64_t)gte->BK[0] << 12; T[1] = (int64_t)gte->BK[1] << 12; T[2] = (int64_t)gte->BK[2] << 12; break;
        case 2: T[0] = (int64_t)gte->FC[0] << 12; T[1] = (int64_t)gte->FC[1] << 12; T[2] = (int64_t)gte->FC[2] << 12; break;
        case 3: T[0] = 0; T[1] = 0; T[2] = 0; break;
    }

    // Multiply
    int64_t mac1 = T[0] + (int64_t)M[0][0] * V[0] + (int64_t)M[0][1] * V[1] + (int64_t)M[0][2] * V[2];
    int64_t mac2 = T[1] + (int64_t)M[1][0] * V[0] + (int64_t)M[1][1] * V[1] + (int64_t)M[1][2] * V[2];
    int64_t mac3 = T[2] + (int64_t)M[2][0] * V[0] + (int64_t)M[2][1] * V[1] + (int64_t)M[2][2] * V[2];

    if (sf) {
        mac1 >>= 12; mac2 >>= 12; mac3 >>= 12;
    }

    gte->check_mac_overflow(mac1, 1);
    gte->check_mac_overflow(mac2, 2);
    gte->check_mac_overflow(mac3, 3);

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// OP (0x0C) — Outer Product of two vectors (cross product)
// ---------------------------------------------------------------------------
void gte_op(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;
    // D1=RT[0][0], D2=RT[1][1], D3=RT[2][2] (rotation matrix diagonal)
    int16_t d1 = gte->RT[0][0];
    int16_t d2 = gte->RT[1][1];
    int16_t d3 = gte->RT[2][2];

    int64_t mac1 = (int64_t)d2 * gte->IR3 - (int64_t)d3 * gte->IR2;
    int64_t mac2 = (int64_t)d3 * gte->IR1 - (int64_t)d1 * gte->IR3;
    int64_t mac3 = (int64_t)d1 * gte->IR2 - (int64_t)d2 * gte->IR1;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// SQR (0x28) — Square of IR vector
// ---------------------------------------------------------------------------
void gte_sqr(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;

    int64_t mac1 = (int64_t)gte->IR1 * gte->IR1;
    int64_t mac2 = (int64_t)gte->IR2 * gte->IR2;
    int64_t mac3 = (int64_t)gte->IR3 * gte->IR3;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// GPF (0x3D) — General Purpose Interpolation (IR0 * IR)
// ---------------------------------------------------------------------------
void gte_gpf(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;

    int64_t mac1 = (int64_t)gte->IR0 * gte->IR1;
    int64_t mac2 = (int64_t)gte->IR0 * gte->IR2;
    int64_t mac3 = (int64_t)gte->IR0 * gte->IR3;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// GPL (0x3E) — General Purpose Interpolation (MAC + IR0 * IR)
// ---------------------------------------------------------------------------
void gte_gpl(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;

    int64_t mac1 = ((int64_t)gte->MAC1 << (sf ? 12 : 0)) + (int64_t)gte->IR0 * gte->IR1;
    int64_t mac2 = ((int64_t)gte->MAC2 << (sf ? 12 : 0)) + (int64_t)gte->IR0 * gte->IR2;
    int64_t mac3 = ((int64_t)gte->MAC3 << (sf ? 12 : 0)) + (int64_t)gte->IR0 * gte->IR3;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// MTC2 — Move To Coprocessor 2 (write GTE data register)
// ---------------------------------------------------------------------------
void gte_mtc2(GTEState* gte, uint8_t reg, uint32_t value) {
    switch (reg) {
        case 0:  gte->V0[0] = value & 0xFFFF; gte->V0[1] = value >> 16; break;
        case 1:  gte->V0[2] = value & 0xFFFF; break;
        case 2:  gte->V1[0] = value & 0xFFFF; gte->V1[1] = value >> 16; break;
        case 3:  gte->V1[2] = value & 0xFFFF; break;
        case 4:  gte->V2[0] = value & 0xFFFF; gte->V2[1] = value >> 16; break;
        case 5:  gte->V2[2] = value & 0xFFFF; break;
        case 6:  gte->RGBC = value; break;
        case 7:  gte->OTZ = value & 0xFFFF; break;
        case 8:  gte->IR0 = static_cast<int16_t>(value & 0xFFFF); break;
        case 9:  gte->IR1 = static_cast<int16_t>(value & 0xFFFF); break;
        case 10: gte->IR2 = static_cast<int16_t>(value & 0xFFFF); break;
        case 11: gte->IR3 = static_cast<int16_t>(value & 0xFFFF); break;
        case 12: gte->SXY[0] = value; break;
        case 13: gte->SXY[1] = value; break;
        case 14: gte->SXY[2] = value; gte->SXY[3] = value; break;
        case 15: // SXYP — push to FIFO
            gte->SXY[0] = gte->SXY[1]; gte->SXY[1] = gte->SXY[2];
            gte->SXY[2] = value; gte->SXY[3] = value; break;
        case 16: gte->SZ[0] = value & 0xFFFF; break;
        case 17: gte->SZ[1] = value & 0xFFFF; break;
        case 18: gte->SZ[2] = value & 0xFFFF; break;
        case 19: gte->SZ[3] = value & 0xFFFF; break;
        case 20: gte->RGB[0] = value; break;
        case 21: gte->RGB[1] = value; break;
        case 22: gte->RGB[2] = value; break;
        case 23: break; // RES1 reserved
        case 24: gte->MAC0 = value; break;
        case 25: gte->MAC1 = value; break;
        case 26: gte->MAC2 = value; break;
        case 27: gte->MAC3 = value; break;
        case 28: // IRGB — packed 5-bit RGB → IR1/IR2/IR3
            gte->IR1 = (value & 0x1F) << 7;
            gte->IR2 = ((value >> 5) & 0x1F) << 7;
            gte->IR3 = ((value >> 10) & 0x1F) << 7;
            break;
        case 29: break; // ORGB read-only
        case 30: // LZCS — set source and compute LZCR
            gte->LZCS = static_cast<int32_t>(value);
            {
                uint32_t v = value;
                if ((int32_t)value < 0) v = ~v;
                int cnt = 0;
                if (v == 0) { cnt = 32; }
                else { while (!(v & 0x80000000u)) { v <<= 1; cnt++; } }
                gte->LZCR = cnt;
            }
            break;
        case 31: break; // LZCR read-only
        default: break;
    }
}

// ---------------------------------------------------------------------------
// MFC2 — Move From Coprocessor 2 (read GTE data register)
// ---------------------------------------------------------------------------
uint32_t gte_mfc2(GTEState* gte, uint8_t reg) {
    switch (reg) {
        case 0:  return (static_cast<uint16_t>(gte->V0[1]) << 16) | static_cast<uint16_t>(gte->V0[0]);
        case 1:  return static_cast<uint16_t>(gte->V0[2]);
        case 2:  return (static_cast<uint16_t>(gte->V1[1]) << 16) | static_cast<uint16_t>(gte->V1[0]);
        case 3:  return static_cast<uint16_t>(gte->V1[2]);
        case 4:  return (static_cast<uint16_t>(gte->V2[1]) << 16) | static_cast<uint16_t>(gte->V2[0]);
        case 5:  return static_cast<uint16_t>(gte->V2[2]);
        case 6:  return gte->RGBC;
        case 7:  return gte->OTZ;
        case 8:  return static_cast<int32_t>(gte->IR0);  // sign-extend
        case 9:  return static_cast<int32_t>(gte->IR1);
        case 10: return static_cast<int32_t>(gte->IR2);
        case 11: return static_cast<int32_t>(gte->IR3);
        case 12: return gte->SXY[0];
        case 13: return gte->SXY[1];
        case 14: return gte->SXY[2];
        case 15: return gte->SXY[3];
        case 16: return gte->SZ[0];
        case 17: return gte->SZ[1];
        case 18: return gte->SZ[2];
        case 19: return gte->SZ[3];
        case 20: return gte->RGB[0];
        case 21: return gte->RGB[1];
        case 22: return gte->RGB[2];
        case 23: return 0; // RES1
        case 24: return gte->MAC0;
        case 25: return gte->MAC1;
        case 26: return gte->MAC2;
        case 27: return gte->MAC3;
        case 28: case 29: { // IRGB/ORGB
            uint32_t r = std::clamp((int)(gte->IR1 >> 7), 0, 0x1F);
            uint32_t g = std::clamp((int)(gte->IR2 >> 7), 0, 0x1F);
            uint32_t b = std::clamp((int)(gte->IR3 >> 7), 0, 0x1F);
            return (b << 10) | (g << 5) | r;
        }
        case 30: return gte->LZCS;
        case 31: return gte->LZCR;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// CTC2 — Move To Coprocessor 2 Control (write GTE control register)
// ---------------------------------------------------------------------------
void gte_ctc2(GTEState* gte, uint8_t reg, uint32_t value) {
    switch (reg) {
        case 0:  gte->RT[0][0] = value & 0xFFFF; gte->RT[0][1] = value >> 16; break;
        case 1:  gte->RT[0][2] = value & 0xFFFF; gte->RT[1][0] = value >> 16; break;
        case 2:  gte->RT[1][1] = value & 0xFFFF; gte->RT[1][2] = value >> 16; break;
        case 3:  gte->RT[2][0] = value & 0xFFFF; gte->RT[2][1] = value >> 16; break;
        case 4:  gte->RT[2][2] = value & 0xFFFF; break;
        case 5:  gte->TR[0] = value; break;
        case 6:  gte->TR[1] = value; break;
        case 7:  gte->TR[2] = value; break;
        case 8:  gte->L[0][0] = value & 0xFFFF; gte->L[0][1] = value >> 16; break;
        case 9:  gte->L[0][2] = value & 0xFFFF; gte->L[1][0] = value >> 16; break;
        case 10: gte->L[1][1] = value & 0xFFFF; gte->L[1][2] = value >> 16; break;
        case 11: gte->L[2][0] = value & 0xFFFF; gte->L[2][1] = value >> 16; break;
        case 12: gte->L[2][2] = value & 0xFFFF; break;
        case 13: gte->BK[0] = value; break;
        case 14: gte->BK[1] = value; break;
        case 15: gte->BK[2] = value; break;
        case 16: gte->LC[0][0] = value & 0xFFFF; gte->LC[0][1] = value >> 16; break;
        case 17: gte->LC[0][2] = value & 0xFFFF; gte->LC[1][0] = value >> 16; break;
        case 18: gte->LC[1][1] = value & 0xFFFF; gte->LC[1][2] = value >> 16; break;
        case 19: gte->LC[2][0] = value & 0xFFFF; gte->LC[2][1] = value >> 16; break;
        case 20: gte->LC[2][2] = value & 0xFFFF; break;
        case 21: gte->FC[0] = value; break;
        case 22: gte->FC[1] = value; break;
        case 23: gte->FC[2] = value; break;
        case 24: gte->OFX = value; break;
        case 25: gte->OFY = value; break;
        case 26: gte->H = value & 0xFFFF; break;
        case 27: gte->DQA = static_cast<int16_t>(value & 0xFFFF); break;
        case 28: gte->DQB = value; break;
        case 29: gte->ZSF3 = static_cast<int16_t>(value & 0xFFFF); break;
        case 30: gte->ZSF4 = static_cast<int16_t>(value & 0xFFFF); break;
        case 31: gte->FLAG = value & 0x7FFFF000u; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// CFC2 — Move From Coprocessor 2 Control (read GTE control register)
// ---------------------------------------------------------------------------
uint32_t gte_cfc2(GTEState* gte, uint8_t reg) {
    switch (reg) {
        case 0:  return (static_cast<uint16_t>(gte->RT[0][1]) << 16) | static_cast<uint16_t>(gte->RT[0][0]);
        case 1:  return (static_cast<uint16_t>(gte->RT[1][0]) << 16) | static_cast<uint16_t>(gte->RT[0][2]);
        case 2:  return (static_cast<uint16_t>(gte->RT[1][2]) << 16) | static_cast<uint16_t>(gte->RT[1][1]);
        case 3:  return (static_cast<uint16_t>(gte->RT[2][1]) << 16) | static_cast<uint16_t>(gte->RT[2][0]);
        case 4:  return static_cast<uint16_t>(gte->RT[2][2]);
        case 5:  return gte->TR[0];
        case 6:  return gte->TR[1];
        case 7:  return gte->TR[2];
        case 8:  return (static_cast<uint16_t>(gte->L[0][1]) << 16) | static_cast<uint16_t>(gte->L[0][0]);
        case 9:  return (static_cast<uint16_t>(gte->L[1][0]) << 16) | static_cast<uint16_t>(gte->L[0][2]);
        case 10: return (static_cast<uint16_t>(gte->L[1][2]) << 16) | static_cast<uint16_t>(gte->L[1][1]);
        case 11: return (static_cast<uint16_t>(gte->L[2][1]) << 16) | static_cast<uint16_t>(gte->L[2][0]);
        case 12: return static_cast<uint16_t>(gte->L[2][2]);
        case 13: return gte->BK[0];
        case 14: return gte->BK[1];
        case 15: return gte->BK[2];
        case 16: return (static_cast<uint16_t>(gte->LC[0][1]) << 16) | static_cast<uint16_t>(gte->LC[0][0]);
        case 17: return (static_cast<uint16_t>(gte->LC[1][0]) << 16) | static_cast<uint16_t>(gte->LC[0][2]);
        case 18: return (static_cast<uint16_t>(gte->LC[1][2]) << 16) | static_cast<uint16_t>(gte->LC[1][1]);
        case 19: return (static_cast<uint16_t>(gte->LC[2][1]) << 16) | static_cast<uint16_t>(gte->LC[2][0]);
        case 20: return static_cast<uint16_t>(gte->LC[2][2]);
        case 21: return gte->FC[0];
        case 22: return gte->FC[1];
        case 23: return gte->FC[2];
        case 24: return gte->OFX;
        case 25: return gte->OFY;
        case 26: return gte->H;
        case 27: return static_cast<int32_t>(gte->DQA); // sign-extend
        case 28: return gte->DQB;
        case 29: return static_cast<int32_t>(gte->ZSF3);
        case 30: return static_cast<int32_t>(gte->ZSF4);
        case 31: return gte->FLAG;
        default: return 0;
    }
}

} // namespace GTE
} // namespace PSXRecomp

// ---------------------------------------------------------------------------
// gte_execute — C-linkage bridge called from generated code
// ---------------------------------------------------------------------------
static uint64_t s_gte_exec_count = 0;
extern "C" uint64_t gte_get_exec_count(void) { return s_gte_exec_count; }

extern "C" void gte_execute(CPUState* cpu, uint32_t cmd) {
    using namespace PSXRecomp::GTE;
    s_gte_exec_count++;

    GTEState gte;
    // Skip reg 15 (SXYP: push-write, would corrupt SXY FIFO) and
    // reg 28 (IRGB: overwrites IR1/2/3 with lossy 5-bit values; use regs 9-11 instead)
    for (int i = 0; i < 32; i++) {
        if (i == 15 || i == 28) continue;
        gte_mtc2(&gte, i, cpu->gte_data[i]);
    }
    for (int i = 0; i < 32; i++) gte_ctc2(&gte, i, cpu->gte_ctrl[i]);

    uint8_t func = cmd & 0x3F;
    switch (func) {
        case 0x01: gte_rtps(&gte, cmd); break;
        case 0x06: gte_nclip(&gte, cmd); break;
        case 0x0C: gte_op(&gte, cmd); break;
        case 0x10: gte_dpcs(&gte, cmd); break;
        case 0x11: gte_intpl(&gte, cmd); break;
        case 0x12: gte_mvmva(&gte, cmd); break;
        case 0x13: gte_ncds(&gte, cmd); break;
        case 0x14: gte_cdp(&gte, cmd); break;
        case 0x16: gte_ncdt(&gte, cmd); break;
        case 0x1B: gte_nccs(&gte, cmd); break;
        case 0x1C: gte_cc(&gte, cmd); break;
        case 0x1E: gte_ncs(&gte, cmd); break;
        case 0x20: gte_nct(&gte, cmd); break;
        case 0x28: gte_sqr(&gte, cmd); break;
        case 0x29: gte_dpcl(&gte, cmd); break;
        case 0x2A: gte_dpct(&gte, cmd); break;
        case 0x2D: gte_avsz3(&gte, cmd); break;
        case 0x2E: gte_avsz4(&gte, cmd); break;
        case 0x30: gte_rtpt(&gte, cmd); break;
        case 0x3D: gte_gpf(&gte, cmd); break;
        case 0x3E: gte_gpl(&gte, cmd); break;
        case 0x3F: gte_ncct(&gte, cmd); break;
        default:
            exit(1);
            break;
    }

    for (int i = 0; i < 32; i++) cpu->gte_data[i] = gte_mfc2(&gte, i);
    for (int i = 0; i < 32; i++) cpu->gte_ctrl[i] = gte_cfc2(&gte, i);

#ifdef PSX_ENABLE_BLOCK_CYCLES
    /* Faithful GTE command completion-stall: arm the per-command deadline
     * (serializing back-to-back ops). Any later COP2 register access stalls to
     * it. Single shared site for BOTH backends (compiled + dirty interp both
     * route GTE commands through gte_execute). */
    psx_gte_set(cpu, psx_gte_cmd_latency(cmd));
#endif
}

/* C-callable wrappers for GTE register transfers */
// Helper: load GTEState from CPUState, skipping push-write and lossy registers
static void gte_load_data(PSXRecomp::GTE::GTEState& gte, CPUState* cpu) {
    using namespace PSXRecomp::GTE;
    for (int i = 0; i < 32; i++) {
        if (i == 15 || i == 28) continue;  // SXYP (push) and IRGB (lossy overwrite)
        gte_mtc2(&gte, i, cpu->gte_data[i]);
    }
    for (int i = 0; i < 32; i++) gte_ctc2(&gte, i, cpu->gte_ctrl[i]);
    /* Loading emulator state is not a guest CTC2 write. CTC2 to FLAG masks
     * bit 31, but a previously computed FLAG error bit must survive reads and
     * unrelated control-register writes. */
    gte.FLAG = cpu->gte_ctrl[31];
}

extern "C" uint32_t gte_read_data(CPUState* cpu, uint8_t reg) {
    using namespace PSXRecomp::GTE;
    GTEState gte;
    gte_load_data(gte, cpu);
    return gte_mfc2(&gte, reg);
}

extern "C" uint32_t gte_read_ctrl(CPUState* cpu, uint8_t reg) {
    using namespace PSXRecomp::GTE;
    GTEState gte;
    gte_load_data(gte, cpu);
    return gte_cfc2(&gte, reg);
}

extern "C" void gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val) {
    using namespace PSXRecomp::GTE;
    GTEState gte;
    gte_load_data(gte, cpu);
    gte_mtc2(&gte, reg, val);
    for (int i = 0; i < 32; i++) cpu->gte_data[i] = gte_mfc2(&gte, i);
    for (int i = 0; i < 32; i++) cpu->gte_ctrl[i] = gte_cfc2(&gte, i);
}

extern "C" void gte_write_ctrl(CPUState* cpu, uint8_t reg, uint32_t val) {
    using namespace PSXRecomp::GTE;
    GTEState gte;
    gte_load_data(gte, cpu);
    gte_ctc2(&gte, reg, val);
    for (int i = 0; i < 32; i++) cpu->gte_data[i] = gte_mfc2(&gte, i);
    for (int i = 0; i < 32; i++) cpu->gte_ctrl[i] = gte_cfc2(&gte, i);
}

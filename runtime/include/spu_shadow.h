/* spu_shadow.h — verified-enhancement float re-render of the SPU voice mix.
 *
 * A higher-fidelity reimplementation of the PS1 SPU's per-voice ADPCM playback
 * that runs ALONGSIDE the faithful spu.c hardware model. Where the hardware
 * resamples each voice with a 4-point Gaussian table (deliberately muffled) and
 * truncates the mix to int16 at every step, the shadow keeps everything in
 * float and resamples with a 4-point cubic (Catmull-Rom) kernel — no muffling,
 * no intermediate requantization.
 *
 * It is NOT ground truth. The canon spu.c mix stays the authoritative output
 * AND the verify oracle. The shadow is fed, sample-for-sample, into a
 * ShadowVerifier against the canon stream; it only substitutes after a proven
 * window and reverts LOUDLY (logs DEGRADED) the instant correlation/level
 * breaks. Opt-in via PSX_AUDIO_SHADOW=1, default OFF — with it off, spu_render
 * output is byte-identical to upstream (this module is never entered).
 *
 * This file consumes the live per-voice state that spu.c already maintains via
 * a small read-only accessor (spu_shadow_voice_snapshot); it does not duplicate
 * the SPU register decode. ADSR envelope, key-on/off, and SPU RAM contents all
 * come from the canon model, so the shadow can never diverge in WHICH note
 * plays — only in the interpolation/headroom of HOW it is rendered.
 *
 * See PRINCIPLES.md "Verified-Enhancement HLE Is Allowed; Load-Bearing HLE Is
 * Not" and docs/SHADOW_ENHANCEMENTS.md. ShadowVerifier attribution: see
 * audio_shadow.h.
 */

#ifndef PSXRECOMP_SPU_SHADOW_H
#define PSXRECOMP_SPU_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read-once env gate. Returns true if PSX_AUDIO_SHADOW is set to an on value.
 * Cached after the first call. */
bool spu_shadow_enabled(void);

/* Called once per spu_init() to (re)create verifier + per-voice resampler
 * state. Cheap; safe to call when disabled (no-op). */
void spu_shadow_reset(void);

/* Drive the shadow for one render block. `canon` is the int16 stereo mix that
 * spu.c just produced (frames*2 samples, the authoritative output). The shadow
 * re-renders the same block in float from live voice state, feeds (canon,
 * shadow) to the verifier per frame, and — only while proven — overwrites
 * `canon` in place with the float mix (clamped to int16). If the verifier
 * reverts, it logs a DEGRADED line and leaves `canon` untouched.
 *
 * No-op (canon untouched, byte-identical) when the feature is disabled. */
void spu_shadow_process(int16_t* canon, int frames);

/* Diagnostics for the debug server / tests. */
typedef struct {
    int      enabled;
    int      proven;
    uint64_t pauses;
    float    last_r;       /* last window correlation */
    float    last_ratio;   /* last window level ratio */
    float    gain;         /* calibrated shadow gain */
    char     last_revert[160];
} SpuShadowInfo;
void spu_shadow_get_info(SpuShadowInfo* out);

#ifdef __cplusplus
}
#endif

#endif  /* PSXRECOMP_SPU_SHADOW_H */

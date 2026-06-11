/* gpu_gl_renderer.c — hardware OpenGL renderer backend.
 *
 * PHASE 1 (current): stub. gl_backend_get() returns NULL, so requesting
 * [video] renderer = "opengl" logs a notice and falls back to software (see
 * gpu_render.c).  This keeps the facade + config + build wiring in place and
 * exercised while the real backend is built up.
 *
 * PHASE 2+: this file will create an SDL OpenGL context, hold VRAM as a GPU
 * texture/FBO, rasterize PS1 primitives via shaders at the configured internal
 * resolution, and present.  It will then return a populated GpuRenderBackend
 * table here.  See gpu_render.h for the backend interface.
 *
 * The PS1 GPU model the backend must reproduce: 1024x512 16-bit (RGB555+mask)
 * VRAM; textured polys sample 4/8/15-bit texels via CLUT from VRAM; four
 * semi-transparency blend modes; texture windows; dithering; mask bit; 15/24
 * bit display readout. Coherency (game reads VRAM back) is the hard part —
 * handled via on-demand GPU->CPU download in vram_transfer_out / vram_read. */

#include "gpu_render.h"

const GpuRenderBackend *gl_backend_get(void) {
    /* Phase 1: not yet implemented. */
    return 0;
}

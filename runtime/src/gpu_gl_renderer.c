/* gpu_gl_renderer.c — hardware OpenGL renderer backend.
 *
 * PHASE 2a (current): GL context + present. The window gets a real OpenGL
 * context (created by main.cpp via gl_renderer_init_context) and the final
 * image is uploaded to a GL texture and drawn as a full-screen quad with
 * SDL_GL_SwapWindow — replacing the SDL_Renderer 2D blit for the opengl path.
 *
 * Rasterization in this phase still runs through the software rasterizer: the
 * backend vtable delegates every draw/transfer call to the existing sw_*
 * functions, so the GL path produces a pixel-identical image to software. This
 * validates the GL window/context/present plumbing in isolation.
 *
 * PHASE 2b (next): replace the delegating draw_* members with real GPU
 * rasterization — VRAM as a GL texture/FBO, shaders that sample 4/8/15-bit
 * texels via CLUT, primitives drawn at internal resolution. Only the vtable
 * members change; the context/present scaffolding here stays.
 *
 * Uses legacy GL 1.x (immediate mode, no shader/FBO) for the present quad so
 * Phase 2a needs no GL function loader — just -lopengl32. Phase 2b adds a
 * modern-GL loader via SDL_GL_GetProcAddress. */

#include "gpu_render.h"
#include "gpu_sw_renderer.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>

/* GL 1.2 enums MinGW's <GL/gl.h> may not declare (it ships GL 1.1 prototypes). */
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

/* ---- GL context + present state ---------------------------------------- */
static SDL_Window   *s_win = NULL;
static SDL_GLContext s_ctx = NULL;
static GLuint        s_tex = 0;
static int           s_tex_w = 0, s_tex_h = 0;   /* current texture alloc */

/* Create the GL context on an already-created SDL window (must have been
 * created with SDL_WINDOW_OPENGL). Returns 1 on success, 0 to fall back to
 * the software/SDL_Renderer present path. Called from main.cpp. */
int gl_renderer_init_context(SDL_Window *win) {
    s_win = win;
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    s_ctx = SDL_GL_CreateContext(win);
    if (!s_ctx) {
        fprintf(stdout, "psxrecomp: GL context creation failed (%s) — "
                        "falling back to software present\n", SDL_GetError());
        return 0;
    }
    if (SDL_GL_MakeCurrent(win, s_ctx) != 0) {
        fprintf(stdout, "psxrecomp: SDL_GL_MakeCurrent failed (%s)\n", SDL_GetError());
        SDL_GL_DeleteContext(s_ctx); s_ctx = NULL;
        return 0;
    }
    SDL_GL_SetSwapInterval(1);            /* vsync; the wall-clock pacer still rules timing */

    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    const char *ver = (const char *)glGetString(GL_VERSION);
    fprintf(stdout, "psxrecomp: OpenGL context created (%s)\n", ver ? ver : "?");
    return 1;
}

void gl_renderer_shutdown(void) {
    if (s_ctx) {
        if (s_tex) { glDeleteTextures(1, &s_tex); s_tex = 0; }
        SDL_GL_DeleteContext(s_ctx);
        s_ctx = NULL;
    }
}

/* Upload an ARGB8888 (little-endian: BGRA bytes) image and draw it as a
 * full-screen quad. `linear` selects the upscale filter (antialiasing). */
void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear) {
    if (!s_ctx) return;
    int win_w = 0, win_h = 0;
    SDL_GL_GetDrawableSize(s_win, &win_w, &win_h);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);

    /* (Re)allocate the texture only when the source size changes; otherwise
     * sub-update for speed. ARGB8888 in memory is BGRA byte order. */
    if (src_w != s_tex_w || src_h != s_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, src_w, src_h, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        s_tex_w = src_w; s_tex_h = src_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_w, src_h,
                        GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }

    /* Full-screen quad. GL clip space: (-1,-1) bottom-left. The source image is
     * top-down, so flip V (top row -> +1). */
    glBegin(GL_QUADS);
        glTexCoord2f(0.f, 0.f); glVertex2f(-1.f,  1.f);
        glTexCoord2f(1.f, 0.f); glVertex2f( 1.f,  1.f);
        glTexCoord2f(1.f, 1.f); glVertex2f( 1.f, -1.f);
        glTexCoord2f(0.f, 1.f); glVertex2f(-1.f, -1.f);
    glEnd();

    SDL_GL_SwapWindow(s_win);
}

void gl_renderer_present_blank(void) {
    if (!s_ctx) return;
    int win_w = 0, win_h = 0;
    SDL_GL_GetDrawableSize(s_win, &win_w, &win_h);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(s_win);
}

/* ---- Backend vtable (Phase 2a: delegate rasterization to software) ------ */
static const GpuRenderBackend GL_BACKEND = {
    .name                          = "opengl",
    .init                          = sw_renderer_init,
    .set_scale                     = sw_renderer_set_scale,
    .scale                         = sw_renderer_scale,
    .set_texture_filter            = sw_set_texture_filter,
    .texture_filter                = sw_texture_filter,
    .set_semi_transparency         = sw_set_semi_transparency,
    .set_mask_bits                 = sw_set_mask_bits,
    .set_texture_window            = sw_set_texture_window,
    .set_color_modulation          = sw_set_color_modulation,
    .fill_rect                     = sw_fill_rect,
    .copy_rect                     = sw_copy_rect,
    .draw_flat_triangle            = sw_draw_flat_triangle,
    .draw_gouraud_triangle         = sw_draw_gouraud_triangle,
    .draw_textured_triangle        = sw_draw_textured_triangle,
    .draw_shaded_textured_triangle = sw_draw_shaded_textured_triangle,
    .draw_flat_rect                = sw_draw_flat_rect,
    .draw_textured_rect            = sw_draw_textured_rect,
    .draw_textured_rect_scaled     = sw_draw_textured_rect_scaled,
    .draw_line                     = sw_draw_line,
    .draw_shaded_line              = sw_draw_shaded_line,
    .render_display                = sw_render_display,
    .render_display_hires          = sw_render_display_hires,
    .vram_write                    = sw_vram_write,
    .vram_read                     = sw_vram_read,
    .vram_transfer_in              = sw_vram_transfer_in,
    .vram_transfer_out             = sw_vram_transfer_out,
    .set_draw_area                 = sw_set_draw_area,
    .get_draw_area                 = sw_get_draw_area,
    .set_draw_offset               = sw_set_draw_offset,
};

const GpuRenderBackend *gl_backend_get(void) { return &GL_BACKEND; }

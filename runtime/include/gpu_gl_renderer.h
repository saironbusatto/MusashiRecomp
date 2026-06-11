#ifndef PSX_GPU_GL_RENDERER_H
#define PSX_GPU_GL_RENDERER_H

/* GL backend context + present entry points, called from main.cpp's window
 * setup and present path when [video] renderer = "opengl".  The backend's
 * rasterization vtable is obtained separately via gl_backend_get()
 * (gpu_render.h).  SDL_Window is forward-declared (SDL typedefs it from this
 * same struct tag) so this header needs no SDL include. */

#include <stdint.h>

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

/* Create the GL context on a window made with SDL_WINDOW_OPENGL.
 * Returns 1 on success, 0 to fall back to the SDL_Renderer present path. */
int  gl_renderer_init_context(struct SDL_Window *win);

/* Present an ARGB8888 image (BGRA byte order) as a full-screen quad + swap. */
void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear);

/* Clear to black + swap (display-disabled frame). */
void gl_renderer_present_blank(void);

void gl_renderer_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_GL_RENDERER_H */

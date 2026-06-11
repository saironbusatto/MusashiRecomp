/* gpu_gl_renderer.c — hardware OpenGL renderer backend.
 *
 * PHASE 2b step 1 (current): modern-GL substrate. A GL function loader
 * (SDL_GL_GetProcAddress) plus a shader + VAO present pipeline replaces the
 * legacy immediate-mode quad. The display image is uploaded to a GL texture
 * and drawn with a fullscreen-triangle shader. If modern-GL init fails for any
 * reason, present degrades to the legacy immediate-mode path (never a black
 * screen). Rasterization still runs through software via the backend vtable,
 * so output stays pixel-identical — this isolates and validates the shader
 * plumbing that GPU rasterization (next steps) is built on.
 *
 * PHASE 2b next: VRAM as a GL texture + an FBO at internal resolution;
 * primitive shaders rasterize geometry into the FBO; present samples the FBO.
 *
 * GL usage: GL 1.x (textures, immediate-mode fallback) comes from opengl32;
 * GL 2.0+/3.0+ entry points (shaders, VAO) are loaded at runtime. We avoid the
 * GLchar/GLsizeiptr typedefs (absent from MinGW's <GL/gl.h>) by spelling the
 * loaded prototypes with char/ptrdiff_t, which are ABI-identical. */

#include "gpu_render.h"
#include "gpu_sw_renderer.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <stddef.h>
#include <stdio.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
/* GL 2.0+ enums (not in MinGW GL 1.1 headers). */
#define PSXGL_FRAGMENT_SHADER 0x8B30
#define PSXGL_VERTEX_SHADER   0x8B31
#define PSXGL_COMPILE_STATUS  0x8B81
#define PSXGL_LINK_STATUS     0x8B82
#define PSXGL_TEXTURE0        0x84C0

#ifndef APIENTRY
#define APIENTRY
#endif

/* ---- Loaded modern-GL entry points ------------------------------------- */
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum);
typedef void   (APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void   (APIENTRY *PFN_glCompileShader)(GLuint);
typedef void   (APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void   (APIENTRY *PFN_glDeleteShader)(GLuint);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void   (APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void   (APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void   (APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void   (APIENTRY *PFN_glUseProgram)(GLuint);
typedef GLint  (APIENTRY *PFN_glGetUniformLocation)(GLuint, const char *);
typedef void   (APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef void   (APIENTRY *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glBindVertexArray)(GLuint);
typedef void   (APIENTRY *PFN_glActiveTexture)(GLenum);

static PFN_glCreateShader      p_glCreateShader;
static PFN_glShaderSource      p_glShaderSource;
static PFN_glCompileShader     p_glCompileShader;
static PFN_glGetShaderiv       p_glGetShaderiv;
static PFN_glGetShaderInfoLog  p_glGetShaderInfoLog;
static PFN_glDeleteShader      p_glDeleteShader;
static PFN_glCreateProgram     p_glCreateProgram;
static PFN_glAttachShader      p_glAttachShader;
static PFN_glLinkProgram       p_glLinkProgram;
static PFN_glGetProgramiv      p_glGetProgramiv;
static PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
static PFN_glUseProgram        p_glUseProgram;
static PFN_glGetUniformLocation p_glGetUniformLocation;
static PFN_glUniform1i         p_glUniform1i;
static PFN_glGenVertexArrays   p_glGenVertexArrays;
static PFN_glBindVertexArray   p_glBindVertexArray;
static PFN_glActiveTexture     p_glActiveTexture;

static int load_modern_gl(void) {
    int ok = 1;
#define LOAD(p, n) do { p = (void *)SDL_GL_GetProcAddress(n); if (!p) ok = 0; } while (0)
    LOAD(p_glCreateShader,       "glCreateShader");
    LOAD(p_glShaderSource,       "glShaderSource");
    LOAD(p_glCompileShader,      "glCompileShader");
    LOAD(p_glGetShaderiv,        "glGetShaderiv");
    LOAD(p_glGetShaderInfoLog,   "glGetShaderInfoLog");
    LOAD(p_glDeleteShader,       "glDeleteShader");
    LOAD(p_glCreateProgram,      "glCreateProgram");
    LOAD(p_glAttachShader,       "glAttachShader");
    LOAD(p_glLinkProgram,        "glLinkProgram");
    LOAD(p_glGetProgramiv,       "glGetProgramiv");
    LOAD(p_glGetProgramInfoLog,  "glGetProgramInfoLog");
    LOAD(p_glUseProgram,         "glUseProgram");
    LOAD(p_glGetUniformLocation, "glGetUniformLocation");
    LOAD(p_glUniform1i,          "glUniform1i");
    LOAD(p_glGenVertexArrays,    "glGenVertexArrays");
    LOAD(p_glBindVertexArray,    "glBindVertexArray");
    LOAD(p_glActiveTexture,      "glActiveTexture");
#undef LOAD
    return ok;
}

/* ---- GL context + present state ---------------------------------------- */
static SDL_Window   *s_win = NULL;
static SDL_GLContext s_ctx = NULL;
static GLuint        s_tex = 0;
static int           s_tex_w = 0, s_tex_h = 0;
static int           s_modern_ok = 0;     /* shader pipeline available */
static GLuint        s_prog = 0;          /* present shader program */
static GLuint        s_vao  = 0;          /* empty VAO for the fullscreen triangle */
static GLint         s_uTex = -1;

static const char *PRESENT_VS =
    "#version 330\n"
    "out vec2 v_uv;\n"
    "void main(){\n"
    "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"  /* (0,0)(2,0)(0,2) */
    "  v_uv = vec2(p.x, 1.0 - p.y);\n"                              /* flip V: src is top-down */
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *PRESENT_FS =
    "#version 330\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 frag;\n"
    "void main(){ frag = texture(u_tex, v_uv); }\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, PSXGL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; log[0] = 0;
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stdout, "psxrecomp: GL shader compile failed: %s\n", log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint build_program(const char *vs_src, const char *fs_src) {
    GLuint vs = compile_shader(PSXGL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(PSXGL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    p_glLinkProgram(prog);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(prog, PSXGL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; log[0] = 0;
        p_glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stdout, "psxrecomp: GL program link failed: %s\n", log);
        return 0;
    }
    return prog;
}

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
    SDL_GL_SetSwapInterval(1);

    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    const char *ver = (const char *)glGetString(GL_VERSION);
    fprintf(stdout, "psxrecomp: OpenGL context created (%s)\n", ver ? ver : "?");

    /* Modern-GL present pipeline (optional; falls back to immediate mode). */
    s_modern_ok = load_modern_gl();
    if (s_modern_ok) {
        s_prog = build_program(PRESENT_VS, PRESENT_FS);
        if (s_prog) {
            p_glGenVertexArrays(1, &s_vao);
            s_uTex = p_glGetUniformLocation(s_prog, "u_tex");
        } else {
            s_modern_ok = 0;
        }
    }
    fprintf(stdout, "psxrecomp: GL present path = %s\n",
            s_modern_ok ? "shader" : "immediate(legacy)");
    return 1;
}

void gl_renderer_shutdown(void) {
    if (s_ctx) {
        if (s_tex) { glDeleteTextures(1, &s_tex); s_tex = 0; }
        SDL_GL_DeleteContext(s_ctx);
        s_ctx = NULL;
    }
}

/* Upload the ARGB8888 (BGRA byte order) image into s_tex (alloc on size change). */
static void upload_present_tex(const uint32_t *pixels, int src_w, int src_h, int linear) {
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    if (src_w != s_tex_w || src_h != s_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, src_w, src_h, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        s_tex_w = src_w; s_tex_h = src_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_w, src_h,
                        GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }
}

void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear) {
    if (!s_ctx) return;
    int win_w = 0, win_h = 0;
    SDL_GL_GetDrawableSize(s_win, &win_w, &win_h);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    p_glActiveTexture(PSXGL_TEXTURE0);
    upload_present_tex(pixels, src_w, src_h, linear);

    if (s_modern_ok) {
        p_glUseProgram(s_prog);
        p_glUniform1i(s_uTex, 0);
        p_glBindVertexArray(s_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);   /* fullscreen triangle */
        p_glBindVertexArray(0);
        p_glUseProgram(0);
    } else {
        /* Legacy fallback: textured fullscreen quad (V already flipped here). */
        glEnable(GL_TEXTURE_2D);
        glBegin(GL_QUADS);
            glTexCoord2f(0.f, 0.f); glVertex2f(-1.f,  1.f);
            glTexCoord2f(1.f, 0.f); glVertex2f( 1.f,  1.f);
            glTexCoord2f(1.f, 1.f); glVertex2f( 1.f, -1.f);
            glTexCoord2f(0.f, 1.f); glVertex2f(-1.f, -1.f);
        glEnd();
    }

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

/* ---- Backend vtable (rasterization still delegated to software) --------- */
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

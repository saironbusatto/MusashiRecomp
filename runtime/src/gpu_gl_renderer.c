/* gpu_gl_renderer.c — hardware OpenGL renderer backend.
 *
 * PHASE 2b step 2 (current): first real GPU rasterization. VRAM is mirrored as
 * a GL texture bound to an FBO (the render target); OPAQUE flat and gouraud
 * triangles are rasterized on the GPU into that FBO. Everything else —
 * textured polys, rects/sprites, lines, fills, semi-transparent polys, VRAM
 * transfers/copies — still runs on the software rasterizer over CPU VRAM.
 *
 * Coherency: CPU VRAM and the FBO are kept in sync with two dirty flags. A GPU
 * draw first uploads CPU VRAM if the CPU side is ahead; a software op (or the
 * display readout) first reads the FBO back if the GPU side is ahead. So the
 * two paths interleave correctly in PS1 draw order. The sync lives entirely in
 * the backend vtable — main.cpp/present are unchanged (they still read the
 * software display via render_display, which syncs the FBO down first).
 *
 * This per-op full-VRAM sync is intentionally simple, not fast: with most of
 * Tomba's drawing still software (textured), it thrashes and runs slow. That's
 * expected for this step — the win arrives once textured polys move to the GPU
 * too (next steps), at which point the sync largely disappears.
 *
 * Present pipeline + GL loader: see the modern-GL section below (shared with
 * step 1). Legacy immediate-mode present remains the fallback. */

#include "gpu_render.h"
#include "gpu_sw_renderer.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
/* GL 2.0+/3.0 enums absent from MinGW's GL 1.1 headers. */
#define PSXGL_FRAGMENT_SHADER     0x8B30
#define PSXGL_VERTEX_SHADER       0x8B31
#define PSXGL_COMPILE_STATUS      0x8B81
#define PSXGL_LINK_STATUS         0x8B82
#define PSXGL_TEXTURE0            0x84C0
#define PSXGL_ARRAY_BUFFER        0x8892
#define PSXGL_STREAM_DRAW         0x88E0
#define PSXGL_FRAMEBUFFER         0x8D40
#define PSXGL_COLOR_ATTACHMENT0   0x8CE0
#define PSXGL_FRAMEBUFFER_COMPLETE 0x8CD5

#ifndef APIENTRY
#define APIENTRY
#endif

#define VRAM_W 1024
#define VRAM_H 512

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
typedef void   (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glBufferData)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void   (APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void   (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);
typedef void   (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);

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
static PFN_glGenBuffers        p_glGenBuffers;
static PFN_glBindBuffer        p_glBindBuffer;
static PFN_glBufferData        p_glBufferData;
static PFN_glVertexAttribPointer p_glVertexAttribPointer;
static PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;
static PFN_glGenFramebuffers   p_glGenFramebuffers;
static PFN_glBindFramebuffer   p_glBindFramebuffer;
static PFN_glFramebufferTexture2D p_glFramebufferTexture2D;
static PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus;

static int load_modern_gl(void) {
    int ok = 1;
#define LOAD(p, n) do { p = (void *)SDL_GL_GetProcAddress(n); if (!p) ok = 0; } while (0)
    LOAD(p_glCreateShader, "glCreateShader");   LOAD(p_glShaderSource, "glShaderSource");
    LOAD(p_glCompileShader, "glCompileShader"); LOAD(p_glGetShaderiv, "glGetShaderiv");
    LOAD(p_glGetShaderInfoLog, "glGetShaderInfoLog"); LOAD(p_glDeleteShader, "glDeleteShader");
    LOAD(p_glCreateProgram, "glCreateProgram"); LOAD(p_glAttachShader, "glAttachShader");
    LOAD(p_glLinkProgram, "glLinkProgram");     LOAD(p_glGetProgramiv, "glGetProgramiv");
    LOAD(p_glGetProgramInfoLog, "glGetProgramInfoLog"); LOAD(p_glUseProgram, "glUseProgram");
    LOAD(p_glGetUniformLocation, "glGetUniformLocation"); LOAD(p_glUniform1i, "glUniform1i");
    LOAD(p_glGenVertexArrays, "glGenVertexArrays"); LOAD(p_glBindVertexArray, "glBindVertexArray");
    LOAD(p_glActiveTexture, "glActiveTexture");  LOAD(p_glGenBuffers, "glGenBuffers");
    LOAD(p_glBindBuffer, "glBindBuffer");        LOAD(p_glBufferData, "glBufferData");
    LOAD(p_glVertexAttribPointer, "glVertexAttribPointer");
    LOAD(p_glEnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(p_glGenFramebuffers, "glGenFramebuffers"); LOAD(p_glBindFramebuffer, "glBindFramebuffer");
    LOAD(p_glFramebufferTexture2D, "glFramebufferTexture2D");
    LOAD(p_glCheckFramebufferStatus, "glCheckFramebufferStatus");
#undef LOAD
    return ok;
}

/* ---- state ------------------------------------------------------------- */
static SDL_Window   *s_win = NULL;
static SDL_GLContext s_ctx = NULL;
static uint16_t     *s_vram = NULL;       /* CPU VRAM (authoritative mirror) */

static GLuint        s_present_tex = 0;
static int           s_present_w = 0, s_present_h = 0;
static int           s_modern_ok = 0;
static GLuint        s_present_prog = 0, s_present_vao = 0;
static GLint         s_present_uTex = -1;

/* GPU rasterization target: VRAM as an RGBA8 texture + FBO. */
static int           s_raster_ok = 0;      /* GPU geometry path available */
static GLuint        s_vram_tex = 0;
static GLuint        s_fbo = 0;
static GLuint        s_geo_prog = 0, s_geo_vao = 0, s_geo_vbo = 0;
static int           s_cpu_dirty = 0;      /* CPU VRAM has changes not in FBO */
static int           s_gpu_dirty = 0;      /* FBO has changes not in CPU VRAM */
static uint32_t     *s_conv = NULL;        /* 1024*512 RGBA8 staging for sync */

/* draw state mirrored from the vtable set_* calls (for GPU prims) */
static int s_off_x = 0, s_off_y = 0;
static int s_area_x1 = 0, s_area_y1 = 0, s_area_x2 = VRAM_W - 1, s_area_y2 = VRAM_H - 1;
static int s_semi_en = 0, s_semi_mode = 0;

/* ---- shaders ----------------------------------------------------------- */
static const char *PRESENT_VS =
    "#version 330\n"
    "out vec2 v_uv;\n"
    "void main(){ vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
    "  v_uv = vec2(p.x, 1.0 - p.y); gl_Position = vec4(p*2.0-1.0,0.0,1.0); }\n";
static const char *PRESENT_FS =
    "#version 330\n"
    "in vec2 v_uv; uniform sampler2D u_tex; out vec4 frag;\n"
    "void main(){ frag = texture(u_tex, v_uv); }\n";

/* Geometry: position in VRAM pixels (draw offset already applied), color in
 * 0..1. Transform to clip space over the 1024x512 VRAM; no Y flip (VRAM y=0 ->
 * clip.y=-1 -> FBO row 0 -> glReadPixels row 0 -> CPU VRAM row 0). */
static const char *GEO_VS =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec4 a_col;\n"
    "out vec4 v_col;\n"
    "void main(){ v_col = a_col;\n"
    "  float cx = a_pos.x/512.0 - 1.0;\n"
    "  float cy = a_pos.y/256.0 - 1.0;\n"
    "  gl_Position = vec4(cx, cy, 0.0, 1.0); }\n";
static const char *GEO_FS =
    "#version 330\n"
    "in vec4 v_col; out vec4 frag;\n"
    "void main(){ frag = v_col; }\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, PSXGL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; log[0]=0; p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stdout, "psxrecomp: GL shader compile failed: %s\n", log);
        p_glDeleteShader(s); return 0; }
    return s;
}
static GLuint build_program(const char *vs, const char *fs) {
    GLuint v = compile_shader(PSXGL_VERTEX_SHADER, vs), f = compile_shader(PSXGL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, v); p_glAttachShader(p, f); p_glLinkProgram(p);
    p_glDeleteShader(v); p_glDeleteShader(f);
    GLint ok = 0; p_glGetProgramiv(p, PSXGL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; log[0]=0; p_glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stdout, "psxrecomp: GL program link failed: %s\n", log); return 0; }
    return p;
}

/* ---- pixel conversion (PS1 1555: bit15=mask, B[14:10] G[9:5] R[4:0]) ---- */
static inline uint32_t conv_1555_to_rgba8(uint16_t p) {
    uint32_t r = (p & 0x1F) << 3, g = ((p >> 5) & 0x1F) << 3, b = ((p >> 10) & 0x1F) << 3;
    uint32_t a = (p >> 15) & 1 ? 0xFF : 0;
    return r | (g << 8) | (b << 16) | (a << 24);   /* RGBA8 little-endian */
}
static inline uint16_t conv_rgba8_to_1555(uint32_t c) {
    uint16_t r = (c & 0xFF) >> 3, g = ((c >> 8) & 0xFF) >> 3, b = ((c >> 16) & 0xFF) >> 3;
    uint16_t m = ((c >> 24) & 0x80) ? 0x8000 : 0;
    return (uint16_t)(r | (g << 5) | (b << 10) | m);
}

/* ---- coherency --------------------------------------------------------- */
static void ensure_gpu(void) {   /* make the FBO reflect CPU VRAM */
    if (!s_raster_ok || !s_cpu_dirty) return;
    for (int i = 0; i < VRAM_W * VRAM_H; i++) s_conv[i] = conv_1555_to_rgba8(s_vram[i]);
    glBindTexture(GL_TEXTURE_2D, s_vram_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_W, VRAM_H, GL_RGBA, GL_UNSIGNED_BYTE, s_conv);
    s_cpu_dirty = 0;
}
static void ensure_cpu(void) {   /* make CPU VRAM reflect the FBO */
    if (!s_raster_ok || !s_gpu_dirty) return;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_fbo);
    glReadPixels(0, 0, VRAM_W, VRAM_H, GL_RGBA, GL_UNSIGNED_BYTE, s_conv);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    for (int i = 0; i < VRAM_W * VRAM_H; i++) s_vram[i] = conv_rgba8_to_1555(s_conv[i]);
    s_gpu_dirty = 0;
}

/* Rasterize one triangle (3 verts: x,y in raw coords; colors 1555) on the GPU. */
static void gpu_triangle(int x0,int y0,uint16_t c0, int x1,int y1,uint16_t c1,
                         int x2,int y2,uint16_t c2) {
    ensure_gpu();
    float verts[3*6];
    int xs[3] = {x0+s_off_x, x1+s_off_x, x2+s_off_x};
    int ys[3] = {y0+s_off_y, y1+s_off_y, y2+s_off_y};
    uint16_t cs[3] = {c0,c1,c2};
    for (int i = 0; i < 3; i++) {
        verts[i*6+0] = (float)xs[i];
        verts[i*6+1] = (float)ys[i];
        verts[i*6+2] = ((cs[i] & 0x1F) << 3) / 255.0f;
        verts[i*6+3] = (((cs[i] >> 5) & 0x1F) << 3) / 255.0f;
        verts[i*6+4] = (((cs[i] >> 10) & 0x1F) << 3) / 255.0f;
        verts[i*6+5] = 1.0f;
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, VRAM_W, VRAM_H);
    glEnable(GL_SCISSOR_TEST);
    int sx = s_area_x1, sy = s_area_y1;
    int sw = s_area_x2 - s_area_x1 + 1, sh = s_area_y2 - s_area_y1 + 1;
    if (sw < 0) sw = 0; if (sh < 0) sh = 0;
    glScissor(sx, sy, sw, sh);          /* FBO y matches VRAM y (no flip) */
    glDisable(GL_BLEND);                 /* opaque only in this step */
    p_glUseProgram(s_geo_prog);
    p_glBindVertexArray(s_geo_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts, PSXGL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    s_gpu_dirty = 1;
}

/* ---- backend vtable wrappers ------------------------------------------- */
static void glb_init(uint16_t *vram) { s_vram = vram; sw_renderer_init(vram); }
static void glb_set_scale(int s) { sw_renderer_set_scale(s); }
static int  glb_scale(void) { return sw_renderer_scale(); }
static void glb_set_texture_filter(int b) { sw_set_texture_filter(b); }
static int  glb_texture_filter(void) { return sw_texture_filter(); }

static void glb_set_semi_transparency(int e, int m) { s_semi_en = e; s_semi_mode = m; sw_set_semi_transparency(e, m); }
static void glb_set_mask_bits(int s, int c) { sw_set_mask_bits(s, c); }
static void glb_set_texture_window(uint32_t r) { sw_set_texture_window(r); }
static void glb_set_color_modulation(int r,int g,int b,int raw) { sw_set_color_modulation(r,g,b,raw); }
static void glb_set_draw_area(int x1,int y1,int x2,int y2) { s_area_x1=x1; s_area_y1=y1; s_area_x2=x2; s_area_y2=y2; sw_set_draw_area(x1,y1,x2,y2); }
static void glb_get_draw_area(int *x1,int *y1,int *x2,int *y2) { sw_get_draw_area(x1,y1,x2,y2); }
static void glb_set_draw_offset(int x,int y) { s_off_x=x; s_off_y=y; sw_set_draw_offset(x,y); }

/* GPU-rasterized: opaque flat / gouraud triangles. Semi-transparent -> SW. */
static void glb_draw_flat_triangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t col) {
    if (s_raster_ok && !s_semi_en) { gpu_triangle(x0,y0,col, x1,y1,col, x2,y2,col); return; }
    ensure_cpu(); sw_draw_flat_triangle(x0,y0,x1,y1,x2,y2,col); s_cpu_dirty = 1;
}
static void glb_draw_gouraud_triangle(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1,int x2,int y2,uint16_t c2) {
    if (s_raster_ok && !s_semi_en) { gpu_triangle(x0,y0,c0, x1,y1,c1, x2,y2,c2); return; }
    ensure_cpu(); sw_draw_gouraud_triangle(x0,y0,c0,x1,y1,c1,x2,y2,c2); s_cpu_dirty = 1;
}

/* Everything else: software, with the FBO read back first if it's ahead. */
static void glb_fill_rect(int x,int y,int w,int h,uint16_t c){ ensure_cpu(); sw_fill_rect(x,y,w,h,c); s_cpu_dirty=1; }
static void glb_copy_rect(int sx,int sy,int dx,int dy,int w,int h){ ensure_cpu(); sw_copy_rect(sx,sy,dx,dy,w,h); s_cpu_dirty=1; }
static void glb_draw_textured_triangle(int x0,int y0,int u0,int v0,int x1,int y1,int u1,int v1,int x2,int y2,int u2,int v2,uint16_t cx,uint16_t cy,uint16_t tp){ ensure_cpu(); sw_draw_textured_triangle(x0,y0,u0,v0,x1,y1,u1,v1,x2,y2,u2,v2,cx,cy,tp); s_cpu_dirty=1; }
static void glb_draw_shaded_textured_triangle(int x0,int y0,int u0,int v0,uint32_t c0,int x1,int y1,int u1,int v1,uint32_t c1,int x2,int y2,int u2,int v2,uint32_t c2,uint16_t cx,uint16_t cy,uint16_t tp,int raw){ ensure_cpu(); sw_draw_shaded_textured_triangle(x0,y0,u0,v0,c0,x1,y1,u1,v1,c1,x2,y2,u2,v2,c2,cx,cy,tp,raw); s_cpu_dirty=1; }
static void glb_draw_flat_rect(int x,int y,int w,int h,uint16_t c){ ensure_cpu(); sw_draw_flat_rect(x,y,w,h,c); s_cpu_dirty=1; }
static void glb_draw_textured_rect(int x,int y,int w,int h,int u,int v,uint16_t cx,uint16_t cy,uint16_t tp){ ensure_cpu(); sw_draw_textured_rect(x,y,w,h,u,v,cx,cy,tp); s_cpu_dirty=1; }
static void glb_draw_textured_rect_scaled(int x,int y,int w,int h,int u0,int v0,int u1,int v1,uint16_t cx,uint16_t cy,uint16_t tp){ ensure_cpu(); sw_draw_textured_rect_scaled(x,y,w,h,u0,v0,u1,v1,cx,cy,tp); s_cpu_dirty=1; }
static void glb_draw_line(int x0,int y0,int x1,int y1,uint16_t c){ ensure_cpu(); sw_draw_line(x0,y0,x1,y1,c); s_cpu_dirty=1; }
static void glb_draw_shaded_line(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1){ ensure_cpu(); sw_draw_shaded_line(x0,y0,c0,x1,y1,c1); s_cpu_dirty=1; }
static int  glb_render_display(uint32_t *o,int p,int dx,int dy,int dw,int dh){ ensure_cpu(); return sw_render_display(o,p,dx,dy,dw,dh); }
static int  glb_render_display_hires(uint32_t *o,int p,int dx,int dy,int dw,int dh){ ensure_cpu(); return sw_render_display_hires(o,p,dx,dy,dw,dh); }
static void glb_vram_write(int x,int y,uint16_t px){ ensure_cpu(); sw_vram_write(x,y,px); s_cpu_dirty=1; }
static uint16_t glb_vram_read(int x,int y){ ensure_cpu(); return sw_vram_read(x,y); }
static void glb_vram_transfer_in(int x,int y,int w,int h,const uint16_t *d){ ensure_cpu(); sw_vram_transfer_in(x,y,w,h,d); s_cpu_dirty=1; }
static void glb_vram_transfer_out(int x,int y,int w,int h,uint16_t *d){ ensure_cpu(); sw_vram_transfer_out(x,y,w,h,d); }

/* ---- context init / present -------------------------------------------- */
static void upload_present_tex(const uint32_t *pixels, int w, int h, int linear) {
    glBindTexture(GL_TEXTURE_2D, s_present_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    if (w != s_present_w || h != s_present_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        s_present_w = w; s_present_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }
}

static void init_gpu_raster(void) {
    s_geo_prog = build_program(GEO_VS, GEO_FS);
    if (!s_geo_prog) { fprintf(stdout, "psxrecomp: GL geometry program failed; GPU raster off\n"); return; }
    s_conv = (uint32_t *)malloc((size_t)VRAM_W * VRAM_H * sizeof(uint32_t));
    if (!s_conv) return;
    glGenTextures(1, &s_vram_tex);
    glBindTexture(GL_TEXTURE_2D, s_vram_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, VRAM_W, VRAM_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glGenFramebuffers(1, &s_fbo);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_fbo);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_vram_tex, 0);
    GLenum st = p_glCheckFramebufferStatus(PSXGL_FRAMEBUFFER);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    if (st != PSXGL_FRAMEBUFFER_COMPLETE) { fprintf(stdout, "psxrecomp: GL FBO incomplete (0x%X)\n", st); return; }
    p_glGenVertexArrays(1, &s_geo_vao);
    p_glBindVertexArray(s_geo_vao);
    p_glGenBuffers(1, &s_geo_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(2 * sizeof(float)));
    p_glEnableVertexAttribArray(1);
    p_glBindVertexArray(0);
    s_cpu_dirty = 1;        /* FBO empty; first GPU draw uploads current VRAM */
    s_raster_ok = 1;
    fprintf(stdout, "psxrecomp: GPU rasterization enabled (opaque flat/gouraud polys)\n");
}

int gl_renderer_init_context(SDL_Window *win) {
    s_win = win;
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    s_ctx = SDL_GL_CreateContext(win);
    if (!s_ctx) { fprintf(stdout, "psxrecomp: GL context creation failed (%s)\n", SDL_GetError()); return 0; }
    if (SDL_GL_MakeCurrent(win, s_ctx) != 0) { fprintf(stdout, "psxrecomp: MakeCurrent failed (%s)\n", SDL_GetError()); SDL_GL_DeleteContext(s_ctx); s_ctx=NULL; return 0; }
    SDL_GL_SetSwapInterval(1);
    glGenTextures(1, &s_present_tex);
    glBindTexture(GL_TEXTURE_2D, s_present_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    const char *ver = (const char *)glGetString(GL_VERSION);
    fprintf(stdout, "psxrecomp: OpenGL context created (%s)\n", ver ? ver : "?");

    s_modern_ok = load_modern_gl();
    if (s_modern_ok) {
        s_present_prog = build_program(PRESENT_VS, PRESENT_FS);
        if (s_present_prog) { p_glGenVertexArrays(1, &s_present_vao); s_present_uTex = p_glGetUniformLocation(s_present_prog, "u_tex"); }
        else s_modern_ok = 0;
    }
    fprintf(stdout, "psxrecomp: GL present path = %s\n", s_modern_ok ? "shader" : "immediate(legacy)");
    if (s_modern_ok) init_gpu_raster();
    return 1;
}

void gl_renderer_shutdown(void) {
    if (s_ctx) {
        ensure_cpu();
        if (s_present_tex) glDeleteTextures(1, &s_present_tex);
        if (s_vram_tex) glDeleteTextures(1, &s_vram_tex);
        SDL_GL_DeleteContext(s_ctx); s_ctx = NULL;
    }
    free(s_conv); s_conv = NULL;
}

void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear) {
    if (!s_ctx) return;
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glViewport(0, 0, ww, wh);
    glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    p_glActiveTexture(PSXGL_TEXTURE0);
    upload_present_tex(pixels, src_w, src_h, linear);
    if (s_modern_ok) {
        p_glUseProgram(s_present_prog); p_glUniform1i(s_present_uTex, 0);
        p_glBindVertexArray(s_present_vao); glDrawArrays(GL_TRIANGLES, 0, 3);
        p_glBindVertexArray(0); p_glUseProgram(0);
    } else {
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, s_present_tex);
        glBegin(GL_QUADS);
            glTexCoord2f(0,0); glVertex2f(-1, 1); glTexCoord2f(1,0); glVertex2f(1, 1);
            glTexCoord2f(1,1); glVertex2f(1,-1); glTexCoord2f(0,1); glVertex2f(-1,-1);
        glEnd();
    }
    SDL_GL_SwapWindow(s_win);
}

void gl_renderer_present_blank(void) {
    if (!s_ctx) return;
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glViewport(0, 0, ww, wh); glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(s_win);
}

static const GpuRenderBackend GL_BACKEND = {
    .name = "opengl",
    .init = glb_init, .set_scale = glb_set_scale, .scale = glb_scale,
    .set_texture_filter = glb_set_texture_filter, .texture_filter = glb_texture_filter,
    .set_semi_transparency = glb_set_semi_transparency, .set_mask_bits = glb_set_mask_bits,
    .set_texture_window = glb_set_texture_window, .set_color_modulation = glb_set_color_modulation,
    .fill_rect = glb_fill_rect, .copy_rect = glb_copy_rect,
    .draw_flat_triangle = glb_draw_flat_triangle, .draw_gouraud_triangle = glb_draw_gouraud_triangle,
    .draw_textured_triangle = glb_draw_textured_triangle,
    .draw_shaded_textured_triangle = glb_draw_shaded_textured_triangle,
    .draw_flat_rect = glb_draw_flat_rect, .draw_textured_rect = glb_draw_textured_rect,
    .draw_textured_rect_scaled = glb_draw_textured_rect_scaled,
    .draw_line = glb_draw_line, .draw_shaded_line = glb_draw_shaded_line,
    .render_display = glb_render_display, .render_display_hires = glb_render_display_hires,
    .vram_write = glb_vram_write, .vram_read = glb_vram_read,
    .vram_transfer_in = glb_vram_transfer_in, .vram_transfer_out = glb_vram_transfer_out,
    .set_draw_area = glb_set_draw_area, .get_draw_area = glb_get_draw_area,
    .set_draw_offset = glb_set_draw_offset,
};

const GpuRenderBackend *gl_backend_get(void) { return &GL_BACKEND; }

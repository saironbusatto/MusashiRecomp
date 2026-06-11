# OpenGL Renderer — Handoff (for Fable)

Branch: `feat/gl-renderer` (both psxrecomp and TombaRecomp).
Last commit this session: `b592447` (psxrecomp).
Status: **experimental, opt-in.** Software renderer is still the committed
default; OpenGL is selected per-game via `[video] renderer = "opengl"`.
TombaRecomp `game.toml` has a LOCAL (uncommitted) `renderer = "opengl"` +
`texture_filtering = "bilinear"` override for testing — do NOT commit those;
committed defaults are `software` / `nearest`.

The goal of this work: add a hardware (OpenGL) renderer as a *selectable
option* alongside software, to enable higher internal-resolution scaling
without the software renderer's per-pixel CPU cost. Make it default-with-
software-fallback only once it renders cleanly.

---

## Architecture (as built)

- **Facade + vtable.** `gpu.c` calls `gr_*` (gpu_render.h/.c); a
  `GpuRenderBackend` vtable dispatches to the software (`sw_*`) or OpenGL
  (`glb_*`) backend. `gr_set_backend()` picks at startup from the config.
- **GL backend** lives in `runtime/src/gpu_gl_renderer.c`
  (+ `runtime/include/gpu_gl_renderer.h`). VRAM is mirrored as a GL texture
  (`s_vram_tex`, RGBA8) bound to an FBO (`s_fbo`) — the render target. A
  second raw-16-bit integer texture (`s_vram_raw_tex`, GL_R16UI) is the
  texture-sampling source for textured polys (CLUT decode in-shader).
- **What's on the GPU now:** flat/gouraud triangles; textured triangles
  (4/8/15-bit CLUT, texel-0 discard, PS1 *2-around-0x80 modulation);
  flat/textured rects; fills (scissored glClear); lines (GL_LINES);
  semi-transparency (4 PS1 blend modes via fixed-function blend; textured
  STP via a 2-pass opaque/semi split).
- **Still software** (route through CPU VRAM with FBO sync): `copy_rect`
  (VRAM→VRAM blit), VRAM transfers in/out, single vram read/write.
- **Coherency:** two dirty flags (`s_cpu_dirty`/`s_gpu_dirty`). A GPU draw
  uploads CPU→FBO first if CPU is ahead (`ensure_gpu`); a software op reads
  FBO→CPU first if GPU is ahead (`ensure_cpu`). Both are **full 1 MB**
  syncs today.
- **Present (main.cpp ~line 1118):**
  - If the GPU drew this frame (`gl_renderer_have_gpu_frame()`) and the
    display is 15-bit → `gl_renderer_present_vram()` blits the display
    region from the FBO straight to the window (`glBlitFramebuffer`, no
    readback). This is the fast path.
  - Otherwise (software frame / 24-bit FMV) → `gl_renderer_sync_cpu()` then
    the software readout path (`gpu_display_pixel_argb` → `gl_renderer_present`).
  - `PSX_GL_FORCE_CPU_PRESENT=1` forces the software present path always
    (diagnostic/fallback; also keeps CPU VRAM current every frame).
- Present forces **native resolution** under GL (`glb_scale`/`glb_set_scale`
  return 1) — GPU draws write the FBO at native res only, not the software
  SSAA hi-res mirror `g_hr`. GPU-side hi-res scaling is not implemented yet.

## What works / is verified

- Boots; renderer selectable; `[OpenGL]`/`[Software]` shown in window title.
- BIOS/Sony logos, FMV (MDEC 24-bit intro), and gameplay terrain all render.
- **Double-offset bug fixed** (`846e402`): `gpu.c` pre-applies `draw_offset`
  to every primitive; the GPU triangle paths were adding it again, which
  displaced all GPU geometry off-screen once the camera scrolled (symptom:
  "the entire overworld is gone — only sprites left"). Fixed by not
  re-applying `s_off` in `gpu_triangle`/`gpu_textured_triangle`.
- Loads are faster (user-confirmed) thanks to present-from-FBO removing the
  per-frame readback for GPU scenes.
- Screenshot writes a real PNG (`ee0a454`) and now syncs the FBO down first
  so captures reflect the on-screen frame under FBO-present (`b592447`).

---

## OPEN ISSUES (what looks bad — fix these)

### 1. Menu: vertical line splits the screen; the two halves JITTER badly
- Repro: skip to the LOAD memory-card screen (pink "TOMBA!" tiled bg). A
  vertical seam runs top-to-bottom near center; the two halves shimmer/jitter
  frame-to-frame. "Looks awful."
- **Key evidence:** with `PSX_GL_FORCE_CPU_PRESENT=1` the menu is CLEAN. So
  the artifact is in the *present path*, NOT the GPU render of the frame.
- Switching the FBO present from a crop-shader to `glBlitFramebuffer`
  (`b592447`) did **not** fix it — so it's almost certainly NOT sub-texel
  sampling.
- **Leading hypothesis: the present path ALTERNATES frame-to-frame.** The
  menu interleaves GPU draws (text/sprites) with software `copy_rect` (the
  card-shell UI). Whether a frame ends on a GPU op (`s_gpu_dirty` → FBO/blit
  present) or a software op (`s_cpu_dirty` → software present) flips between
  frames. The two present paths likely differ in scale/position/seam →
  jitter. The vertical seam may be where a `copy_rect` region meets GPU
  content within one path.
- **Suggested fix:** make the FBO the single authoritative surface and use
  ONE present path deterministically. The clean way is to move `copy_rect`
  onto the GPU (FBO self-blit) so the software round-trip — and the path
  flip — disappears. See issue 6.

### 2. Gameplay: persistent horizontal dotted line under Tomba
- A dashed/dotted horizontal line sits at about mid-body height across the
  open area. Persists in the user's view.
- NOT yet isolated to FBO-vs-software present (the attract demo didn't reach
  this scene during the CPU-forced test). First step: reproduce with
  `PSX_GL_FORCE_CPU_PRESENT=1` to see if it's render-side or present-side.
- Candidate causes: a textured-rect/sprite strip seam; missing texture
  window (issue 3); or the same present-path alternation as issue 1.

### 3. Garbled / fuzzy / misaligned text (e.g. the AP counter, top-right)
- Glyphs look smeared, doubled, and misaligned.
- **Leading cause: the GPU textured shader does NOT implement the PS1
  TEXTURE WINDOW** (mask/offset UV wrap). The software path applies it in
  `gpu_sw_renderer.c` `texel_fetch` (`g_tw_mask_x/y`, `g_tw_off_x/y`, the
  `u = (u & ~(mask*8)) | ((off & mask)*8)` masking). `glb_set_texture_window`
  just forwards to SW and is ignored by the GPU shader. Sprites/text that
  rely on UV wrapping render wrong.
- Also: `texture_filtering = "bilinear"` is honored only by the software
  path; the GPU textured shader is nearest-only (issue 5). With bilinear
  requested, the SW-vs-GPU mismatch can compound the fuzzy look.
- **Suggested fix:** pass the texture-window mask/offset as uniforms to the
  textured fragment shader and apply the same masking before the texel
  fetch. Then add an optional bilinear texel fetch (issue 5).

### 4. Fullscreen does not preserve aspect ratio (stretches, no black bars)
- Going fullscreen stretches the image to fill the window instead of
  letterboxing to the PSX display aspect.
- Both present paths map the source to the FULL drawable:
  `gl_renderer_present` (quad over whole window) and
  `gl_renderer_present_vram` (blit dst = `0,wh,ww,0`).
- **Suggested fix:** compute a 4:3 (or correct PSX display aspect) dst rect
  centered in the drawable, clear the whole window to black first, then
  present into the inset rect. Apply to BOTH present functions.

### 5. Bilinear texture filtering not honored on the GPU path
- GPU textured shader does nearest texel fetch only. Software path supports
  `texture_filtering = "bilinear"`. Add a bilinear path in the textured
  fragment shader (manual 4-tap on the integer VRAM texture + CLUT, since
  raw sampling is GL_R16UI/nearest).

### 6. `copy_rect` still software → forces FBO↔CPU round-trips
- This is the structural driver of issue 1 (present-path alternation) and of
  residual lag. Implementing GPU `copy_rect` as an FBO-region self-copy
  (careful with overlap; may need a scratch texture or `glBlitFramebuffer`
  with a temp) lets the FBO stay authoritative and removes the software
  round-trip for menus and scrolling buffers.

### 7. Coherency is full-1 MB sync per transition
- `ensure_gpu`/`ensure_cpu` upload/read the entire 1024×512 VRAM. Once
  copy_rect is on the GPU and the FBO is authoritative, most syncs vanish;
  what remains (texture/CLUT uploads via VRAM transfer) could be reduced to
  dirty sub-rectangles.

### 8. (Deferred goal) GPU-side internal-resolution scaling
- The original ask: "higher scaling without performance loss." Present
  currently forces native res. Once the above render correctly, allocate the
  FBO at N× native, scale GPU draw coordinates, and downsample/blit on
  present. This is the payoff; don't start it until 1–6 are clean.

---

## TASK FOR NEXT SESSION (user-approved, separate from GL)

### Audio fade-in/fade-out at turbo transition windows — APPROVED, implement
- Context: `turbo_loads` runs load screens faster than real time. Tomba's
  load BGM is *sequenced* (CPU writes SPU regs per game-frame), so under
  turbo it speeds up into garble. We MUTE during turbo
  (`main.cpp` ~line 1024–1032: `if (!turbo_loads_active) sdl_audio_pump();`).
- Decoupling audio to "normal pace" during turbo is **NOT feasible** — the
  music's tempo is bound to the frame rate that turbo is compressing
  (analysis in session). The mute stays.
- The mute's hard cut/resume causes awkward gaps. **User approved a quick
  fade-in/fade-out** to smooth the edges (and a small hysteresis so very
  short loads don't flicker the mute on/off).
- Implementation sketch: apply a gain ramp (~30–60 ms) to the SPU output
  when entering/leaving `turbo_loads_active` — ramp down before muting, ramp
  up on resume — rather than hard start/stop of `sdl_audio_pump()`. Apply the
  gain in `spu_render` output or scale the queued `int16_t` samples. Add a
  small minimum-turbo-duration / debounce so brief loads don't audibly dip.

---

## Useful commands / paths

- Build: `PATH=/c/msys64/mingw64/bin:$PATH cmake --build build-stable --target psx-runtime`
  (run from `TombaRecomp/`; the GL source is in `psxrecomp/runtime/`).
- Run: `./build-stable/psx-runtime.exe --game game.toml` (BIOS path baked).
- Debug TCP 4470: `python ../psxrecomp/tools/debug_client.py --port 4470 ping`
  and `... screenshot_file path=foo.png` (now a real PNG, FBO-synced).
- Force software present (diagnostic/fallback): set env
  `PSX_GL_FORCE_CPU_PRESENT=1` before launch.
- Reusable WIP patches live in `psxrecomp/_wip/` (gitignored).
- Attract demo auto-plays gameplay on a cold boot (AP is always 97400 — it's
  the canned demo). Tapping START a few times skips the long intro FMV to the
  title/LOAD menu.

# PSXRecomp — Enhancement-Tier Work (framework-wide)

Faithfulness is the foundation (CLAUDE.md Rule -1); this file tracks the
enhancement layer built on top of it: renderers beyond the software reference,
widescreen, load acceleration, etc. Per-game enhancement ideas live in each
game repo's ENHANCEMENTS.md. Active framework bugs referenced here live in the
game repos' ISSUES.md until a framework tracker exists.

---

## R1 — OpenGL renderer (2nd backend): PLAYABLE, flicker root-caused + fixed

**Status as of 2026-07-03** (branch `feat/renderer-finish`, working tree, uncommitted):

- The long-standing intermittent black-frame flicker (MegaManX6Recomp ISSUES.md
  #7 — the reason MMX6 shipped with the software renderer) is **root-caused and
  fixed**. Mechanism (proven via the new present ring + gl_coh_ring correlation):
  `flush_cpu_upload()` merged all pending CPU→VRAM writes into ONE union bounding
  box; a frame with two disjoint uploads produced a union spanning the display
  framebuffers, which the flush painted from the **stale CPU VRAM mirror** (the
  FBO is authoritative under GL) — stomping live frames with black. Two black
  presents per incident (one per double-buffer parity). Software renderer is
  immune (CPU array is authoritative there), which is why it was the safe default.
- Fix: exact pending-rect list (16 rects; merge only when zero uncovered pixels
  are added; wrap-aware GP0(A0) transfers split into up to 4 exact rects;
  overflow → order-preserving flush-all). Merge rule proven by a 20k-randomized-
  rect host unit test (0 stale / 0 missing painted pixels).
- New always-on observability: **gl_present_ring** (every SwapWindow site records
  path taken, src/letterbox rects, glGetError, wall-ms, backbuffer + blit-source
  pixel samples) — the instrument that made the 1:1 black-capture correlation
  possible. Plus a debug-server fix: send_fmt silently truncated >64KB responses
  into unparseable JSON (broke big ring dumps); now heap-formats exactly.
- Validation: ~18-minute MMX6 GL attract soak, ~1600 window captures across 3+
  full attract cycles — **zero isolated black frames, zero GL errors**, no other
  visual anomalies. Tomba1 build-gl rebuilt with the fix, boots clean (full
  Tomba1 attract soak still owed).

**Remaining to close R1:**
1. Tomba1 full attract-cycle GL soak (village, FMV, HUD).
2. USER eyeball at the MMX6 Rainy Turtloid standing-still spot (the original
   repro; MMX6 build-modern settings.toml is left on renderer="opengl").
3. Flip MMX6's shipping default software→opengl + close ISSUES.md #7.
4. The same union-upload bug exists in the Vulkan backend (see R2 item 1).

## R2 — Vulkan renderer (3rd backend): RENDERS GAMEPLAY AT SPEED, gaps cataloged

**Status as of 2026-07-03** (same branch/tree; `-DPSX_ENABLE_VULKAN=ON`, SDK
1.4.341.1; `feat/vulkan-renderer` turned out to be already merged into master —
only a 26-line build-guard needed salvaging from the retired _wt-vulkan worktree):

- Three bring-up bugs root-caused and fixed this session:
  1. **Boot wedge**: per-pixel GP0 uploads did 2 vkAllocateMemory + 2
     vkQueueSubmit each (driver churn → minutes-long stall, watchdog abort).
     Fixed with GL-style deferred batched uploads.
  2. **Shredded 3D**: draws raced CPU rewrites of the persistently-mapped vertex
     buffer (69.5% pixel divergence vs software at the same guest frame). Fixed
     with sub-allocation cursors + firstVertex bases.
  3. **Semi-transparency order violations** (59.5% divergence): VK still had
     GL's retired whole-batch STP split; ported GL's current two-pass model
     (ordered color pass + color-masked stencil fixup; semi prims isolated).
- Verified (guest-frame-aligned VRAM diffs via the new frameshot.py tool +
  window captures): title pixel-identical to software; attract within 0.62% of
  the GL oracle; **60.5 fps sustained**; vk_perf steady-state ~0-2 allocs and
  ≤6 submits/frame (was thousands). 24-bit FMV present path written (old one
  was provably black) but NOT yet verified on-screen.

**Gap catalog (ranked, low→high effort):**
1. Port the exact-rect pending-upload fix from GL (R1) — VK currently has the
   union model and will inherit the same intermittent stomp/flicker class.
2. Verify the depth24 FMV present path on-screen (vs software reference).
3. Cache the FMV present staging image (currently created/destroyed per frame).
4. Add an explicit depth-stencil VkImageMemoryBarrier between one-shot submits
   (stencil write→test ordering currently rides on color-barrier side effects —
   works on this RTX 3080 Ti, spec-fragile elsewhere).
5. Native-wide (16:9) compositor — entirely missing (wide_* vtable NULL; facade
   reports unsupported and falls back correctly). Mirror GL's per-base wide FBO
   + double-draw + GPU-direct present.
6. SSAA scale >1 unvalidated on VK.
7. Semi-prim isolation perf (one draw per semi triangle; same cost as GL today).

**Validation targets:** MMX6 + Tomba2, agent does initial (window-capture series
+ frame-aligned cross-backend diffs), user does final.

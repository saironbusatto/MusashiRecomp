# BFM Widescreen — Design (Issue #9)

**Status:** designed, not implemented
**Date:** 2026-08-08
**Closes:** ISSUES.md Issue #9 for Brave Fencer Musashi (SLUS-00726)
**Strategy chosen:** squash (the Tomba-proven path), NOT native-wide

---

## Understanding summary

- Ship real 16:9 for BFM via the **squash** strategy, the one Tomba proves in
  production.
- That requires two reverse-engineering findings in the BFM Ghidra corpus: the
  **shared per-prim helper** every character-render function calls with the prim
  pointer in `$a0`, and the **scratchpad address** where the RTPS preamble
  stores the projected anchor SXY.
- Before the RE, build a **Linux window-capture tool**. `screenshot_file` dumps
  the pre-stretch 320 VRAM region, and `wide_shot` (debug_server.c:7253) refuses
  unless native-wide is engaged — so on the squash path nothing in-tree can see
  the composited picture.
- Success is judged by eye on a captured window, not by any internal readout.
  Issue #9 exists precisely because `configured`, `mode` and `squash` all
  reported success while the picture stayed 4:3.
- For: the user playing BFM, and MusaGround downstream, which inherits the
  presentation.

### Non-goals

Native-wide, Issue #8, Stage 2 cycle calibration, and the squash artifact tail
(backdrop void, prop drift). Document if they surface; do not chase.

---

## Why the gate blocks BOTH strategies (the finding that reframed Issue #9)

`gpu.c:124`:

```c
static int ws_game_mode(void) {
    if (ws_full_2d_mode()) return 1;
    return (uint32_t)s_frame_count - ws_last_tag_stamp <= 2;
}
```

`gpu_ws_present_native_43()` returns 1 when `!ws_game_mode()`, and BOTH
`ws_active()` (squash) and `ws_native_wide_active()` (`gpu.c:167`) are gated on
`!gpu_ws_present_native_43()`. With no `[widescreen]` block nothing ever stamps
`ws_last_tag_stamp`, so every frame classifies as a full-2D screen and both
strategies stay off.

The consequence worth writing down: **native-wide does not need per-prim tags to
render** — it shifts `draw_offset` and widens the frame, with no per-primitive
work at all. It needs them only as a *3D-frame-vs-2D-screen detector*. So the
per-game RE that Issue #9 describes is a requirement of the squash strategy
specifically, not of widescreen as such.

This is why **plan B is a general 3D-frame detector (GTE RTPS/RTPT activity this
frame) plus native-wide** — it would unblock every title with zero per-game RE.
Recorded, not chosen: the user chose the proven path.

---

## Assumptions

1. The per-prim callback cost is acceptable — Tomba ships it in production.
2. SDL2 runs under XWayland here (`DISPLAY=:0` is set), so `import -window`
   works. If it is native Wayland, fall back to `SDL_VIDEODRIVER=x11`.
3. "Done" = the user looks at a capture and says it stretched. No internal
   metric is proof; the internal metrics already lied once.
4. **Weakest assumption:** BFM has a recognisable RTPS cluster of
   character-render functions sharing one helper, as Tomba did. If BFM draws
   billboards diffusely there is no "the shared helper" and plan B applies.

---

## Decision log

| # | Decision | Alternatives | Why |
|---|---|---|---|
| 1 | Work on visible/playable output, not foundation | Stage 2 cycle calibration; Issue #8 HLE boot; tooling-debt audit | User's call. Note that the Beetle oracle restored on 2026-08-08 unblocks Stage 2 specifically — §3c named the two-anchor region mode on both backends as its gate, and that now exists. |
| 2 | Squash strategy | Native-wide (repo default, `config_loader.h:406`); general 3D-frame detector | User chose the path proven in production on Tomba. Cost accepted: the artifact tail, and a capture tool that native-wide would not have needed. |
| 3 | Discovery = dynamic narrows, static confirms | Static-only from the Ghidra corpus; dynamic-only correlation | BFM streams gameplay from overlays and overlay corpus coverage is unverified, so static-only bets blind on it. Dynamic-only would pick the anchor address by correlation, and a wrong scratchpad fails silently — the exact Issue #9 failure mode. |
| 4 | Link prims to functions via `wtrace`, not GPU tracing | Correlate `ws_census` against `fntrace` by time | `wtrace` records `func_addr` (dispatch target) and the exact store `pc` per write (debug_server.c:322-327). Direct evidence of who wrote the prim, not temporal coincidence. |
| 5 | Build the capture tool before the RE | User confirms by eye each iteration; do both | User's call: iterate without a human in the loop. |
| 6 | Anchor candidates must pass a runtime test | Accept the address the disassembly implies | Rule 14. A scratchpad that merely correlates fails silently. |

---

## Phases

### Phase 0 — ABANDONED 2026-08-08: no external capture works on this host

Planned as `tools/window_shot.py` grabbing the real window. **It cannot work
here, and the reason is structural — do not try this again on this machine.**

ImageMagick 7's `import` rejects its own documented invocation
("missing an image filename"), so the backend became ffmpeg `x11grab`. That
runs, and returns black. The decisive measurement is a full-screen grab:

```
ffmpeg -f x11grab -video_size 1366x768 -i :0.0+0,0 -frames:v 1 full.png
  -> 1366x768, 43 distinct colours, mean 0.000056
```

An entirely black desktop while the user sees windows normally. This session is
Wayland; XWayland hands its surfaces to the Wayland compositor and the X screen
stays empty, so **no X11-based tool can capture anything**, whatever the window.

A false retraction is worth recording, because it cost a cycle: the hypothesis
was raised, then withdrawn when a user screenshot showed the same black window
the tool had captured — which looked like confirmation the capture was faithful.
It was not; both were failures, and only the full-screen grab separated them.
**When a capture and a screenshot agree on "black", that is not corroboration.**

What survives, and is worth reusing: the degeneracy gate must count DISTINCT
COLOURS, not standard deviation. A black window containing only the mouse cursor
measures stddev 0.0124 — past any sane threshold, because ~200 white pixels in
307200 are enough — but only 43 colours, against thousands for any real PSX
frame (dithered 15-bit output never yields a small palette).

Replacement not yet chosen. The candidates are a `present_shot` debug command
(glReadPixels of the window framebuffer before SwapWindow, reusing
`png_write_rgb`; Wayland-proof and Rule-3-correct), or user-by-eye verification.
Currently the user verifies by eye.

#### 4:3 baseline, captured 2026-08-08 (the reference for "did it get fatter?")

User-confirmed: Musashi at normal proportions, intro/title/menu all correct.

- Game image **884x663** — exactly 4:3 — inside a 933x663 client area.
- The configured 1280 width was cut by `clamp_window_aspect` (main.cpp:224)
  because 4:3 at 1280 is 1280x960 and the screen is 1366x768. **16:9 at 1280 is
  1280x720 and does fit**, so the widescreen window should end up larger, not
  smaller.
- `gpu_state` → `ws:{configured:0, active:0, game_mode:0, mode:0, squash:[1,1]}`.
- `gp0_copy` = 5132, non-zero: BFM does VRAM→VRAM copies. `NATIVE_WIDE_PLAN.md`
  flags framebuffer feedback as the key complexity driver for the wide
  compositor surface, so this matters to plan B.

#### Tooling defect found in passing

`gpu_state` sends its response with no trailing newline, against the documented
line protocol every other handler follows — a readline-based client hangs on it.

### Phase 1 — Discovery

Entry point: savestate **slot 1** (Lumina Rotation tutorial, Musashi + NPC on
screen), working since `e6eba2d`, so every iteration starts from one frame.

1. `ws_census` (always-on, `gpu.c:2568`) gives opcode + coords of drawn prims.
   Coordinates tracking Musashi identify which prims are his.
2. Arm `wtrace_range` over the prim-buffer region; read back `func_addr` per
   store. Functions writing textured-polygon headers during gameplay *are* the
   character renderers, by construction.
3. Open those addresses in `musashi_decomp.c` (or `ghidra_overlay/` if
   overlay-resident). Find the common callee taking the prim pointer in `$a0`,
   and the RTPS preamble's scratchpad store of the projected SXY.

**Acceptance for the anchor:** arm `wtrace` on the candidate and confirm it
changes once per character per frame AND that the value tracks Musashi's screen
position while walking. Correlation alone is rejected.

**BFM-specific hazard:** `sprite_tag_funcs` are static addresses, but overlays
reuse address ranges. If the renderer sits above `0x80074800`, confirm only one
overlay maps that address, or the tag fires inside unrelated code.

### Phase 2 — Apply and verify

`game.toml`: `aspect_ratio = "16:9"`, the `[widescreen]` block, and
**`native_wide = false`** — without it the mode stays 2 and squash never runs
while every indicator reports success.

Regen: `psxrecomp-game --config game.toml`, then `compile_overlays.py` (already
passes `--ws-config`, lines 1349/1758), then build the runtime.

Verification, weakest to strongest:

1. `gpu_state` → `ws.game_mode`. This is the readout that was false in Issue #9
   (stuck at 0). If it is 1 during gameplay, tags are firing.
2. `window_shot` before/after, compared side by side. This is the proof.

**Regression test:** with `aspect_ratio` removed, `screenshot_file` on the same
savestate must be pixel-identical before and after the regen — proving the tag
emit did not alter the 4:3 path, which is the guarantee owed to every other
title.

**Edge cases to check explicitly:** FMV must stay pillarboxed (MDEC detector),
and 2D screens (title, save menu) must not stretch. Both share the predicate, so
both are likely silent regressions.

**Rollback:** revert the toml and regen. The generated-code change is confined
to the entry of tagged functions.

---

## Open risk

If assumption 4 fails — no single shared per-prim helper in BFM — this design
does not degrade gracefully. The fallback is plan B (general GTE-activity 3D
detector + native-wide), which is a different design, not a patch to this one.

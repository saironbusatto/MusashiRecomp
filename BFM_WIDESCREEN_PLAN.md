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

### Phase 0 — `tools/window_shot.py`

Captures the real `psx-runtime` window to PNG via `import -window`, locating it
by an explicitly-set window title (`--window-title`, `main.cpp:2015`) rather
than guessing.

**It must not trust its own success.** A capture that is all-black or a single
flat colour is rejected loudly — a silent black PNG is the same class of failure
this whole plan exists to eliminate.

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

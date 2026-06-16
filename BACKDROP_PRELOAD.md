# Generic backdrop preload (widescreen) — design + implementation plan

Branch: `feat/ws-backdrop-coverage` (both repos, off master). Goal: in 16:9,
Tomba's far-BACKGROUND tile grid (sky/ground/cloud/flower-field layers) only
generates a camera-centered ~4:3 window of tile columns, so the revealed right
(and a little of the left) margin shows void until the camera moves. User
directive: **eliminate the culling / preload the entire background** (free on
modern HW; the extent is a finite tile table), as a **generic auto-detect** that
works across all scene overlays and is reusable for any PSX game.

## Root cause (fully diagnosed 2026-06-15)

The backdrop generator is **overlay-resident AND scene-specific**: each area's
overlay carries its own copy of a structurally-identical column-window generator
at its own addresses (the village's is `FUN_80116a28`; the sky/flower-field
scene's is `~0x80116800`; the bytes at a given address differ per scene). The
main EXE has NO camX cull — it tail-dispatches to these overlay handlers
(verified by byte-searching every `0x176` consumer in SCUS_942.36).

Per window, the generator computes a camera-windowed column range:
```
camX   = lh/lhu rC, 0x176(rB)              ; camera world-X
(opt)    addiu rC, rC, Koff                ; per-layer parallax bias (varies)
start  = (camX+Koff) / 96                  ; signed reciprocal-multiply:
           lui rM,0x6666 ; ori rM,rM,0x6667 ;  (magic 0x66666667)
           mult rC,rM ; mfhi rH ; sra rQ,rH,5 ; subu rQ,rQ,(rC>>31)
         move rStart, rQ                   ; START finalize
end    = addiu rEnd, rStart, N             ; N = 4:3 window width (8..12; some 44/70)
         <start clamp low>                 ; bgtz->li 1, or bgez->clear 0
         <end clamp high>                  ; slt rEnd,rExt -> addiu rEnd,rExt,-1
                                           ;   (rExt = lbu tile-table length, finite)
                                           ; or slti rEnd,Cext -> li rEnd,Cext-1 (const, finite)
inner loop: for col in [start, end): emit one tile from the finite table
```
The window slides with camX → the 16:9-revealed columns aren't generated. Fix =
force `start=0` (loop from col 0) and `end=full extent` (the existing high clamp
pins it), so the whole finite row is emitted every frame regardless of camera.

## The invariant signature (verified on BOTH the village + flower-field overlays)

Byte/opcode-STABLE anchors (match on these):
- **`0x176` camX half-word load** (`lh`/`lhu rC,0x176(rB)`) — offset invariant.
- **Magic `0x66666667`** — `lui rM,0x6666` + `ori rM,rM,0x6667`. Strongest anchor
  (the /96 reciprocal is specific to these scrollers; gating on the `0x176`
  camera offset essentially eliminates collisions).
- **Divide tail** `mult rC,rM` → `mfhi rH` → `sra rQ,rH,0x5` → `subu rQ,rQ,rSign`
  (shift amount 5 invariant).
- **`addiu rEnd,rStart,N`** end-delta with small N, immediately after the
  `move rStart, <quotient>` start-finalize.
- **End clamp** to the tile-table length register (`slt …,rExt`→`addiu rEnd,rExt,-1`)
  or a literal (`slti …,Cext`→`li rEnd,Cext-1`).

VARIES (read from the matched site, never assume): all registers
(village a2=start/a3=end/t0=mfhi; FF s2=start/s1=end/a2=mfhi), the parallax
offset Koff (and whether a `jal 0x8002d964` cam-jump precedes), N (8/9/10/12 and
44/70), the start-clamp floor idiom (floor 0 vs 1 vs const), the extent source
(register `s2`/`s7` vs literal `0x20`/`0x3f`), and the number of windows per
jumptable case (village 1; FF 1–3, two-layer backdrops).

## Patch (two single-instruction rewrites per window; both confirmed clean)

1. **START finalize** `move rStart, rQ` → force `rStart = 0` when widescreen
   active. The existing low clamp keeps it ≥ floor.
2. **END delta** `addiu rEnd, rStart, N` → force `N = 0x100` (≥ any 8-bit table
   length) when widescreen active. The existing high clamp pins `rEnd` to
   `extent-1`. (For const-extent windows, ≥ the literal ceiling, e.g. 0x3f.)

`extra=0` / widescreen-off must reproduce the original N and the original
`move` exactly (byte-identical faithful build).

Full extent is a finite `lbu` tile-table length (≤255, realistically dozens) or
a small literal → preloading the whole row is bounded and safe (no OOB; the emit
loop writes one display-list slot per column from the finite table).

## Implementation plan

### Recompiler pass (gcc/native path) — `[widescreen.cull] auto_backdrop = true`
A function-level pre-pass in `code_generator.cpp` (sibling to
`func_has_screen_extent_cull`): for each function, find every window via the
invariant (magic 0x66666667 + a `0x176` load feeding the `mult` + the
`sra…,5`/`subu` divide tail + `move rStart,quotient` + `addiu rEnd,rStart,N`).
Record a map `{addr -> {START_ZERO(reg) | END_WIDEN(rs,reg)}}`. `translate_instruction`
consults it: at START_ZERO emit `rStart = psx_ws_backdrop_preload() ? 0 : <orig move>`;
at END_WIDEN emit `rEnd = rStart + (psx_ws_backdrop_preload() ? 0x100 : N)`.
`psx_ws_backdrop_preload()` = a runtime predicate (gpu.c), nonzero only when
native-wide is engaged (0 at 4:3 ⇒ byte-identical). Robustness: the function-level
magic+0x176 gate + the divide-quotient dataflow gate make false positives very
unlikely; assert/log the per-function window count at build time.

### Overlay coverage (the generators are overlay-resident)
- The recompiler pass must run for the OVERLAY compile too: `compile_overlays.py`
  already forwards `--ws-config` (game.toml), so `auto_backdrop` reaches it. Bump
  `PSX_OVERLAY_CODEGEN_VER` and rebuild the overlay cache (gcc/<arch-abi>/cgN).
- Backends: a backdrop generator may run as a gcc-cache DLL (pass handles it),
  a sljit shard, or interpreted. For full coverage like `auto_screen_x`, the
  sljit emitter + dirty_ram_interp need the same force-start-0 / widen-end at the
  detected sites. Options: (a) export the detected site map to the runtime so
  sljit/interp can apply it, or (b) re-derive the signature in a sljit/interp
  pre-scan (heavier). NOTE: in dev, overlays often run interpreted — so the
  interp hook is needed to SEE the fix in the debug build.

### Verify
Debug build (build-stable, TCP) navigated to the flower-field/sky-platform scene
(user drives). Confirm the right-margin void fills (full row preloaded) and 4:3
stays byte-identical. Extracted overlay for offline analysis:
`_shots/overlay_flowerfield.bin` (base 0x800E7000); also imported into Ghidra as
`overlay_flowerfield.bin`. Village reference: `overlay_800E7000.bin` /
`_shots/overlay_800E7000.bin`.

## Status
Analysis COMPLETE + cross-verified (this doc). Implementation NOT started.
This is a real recompiler-pass + multi-backend + overlay-cache feature, not the
quick patch it first appeared. Related: `WIDESCREEN.md`,
`NATIVE_WIDE_PLAN.md`, memory `ws_draw_census_8c` / `native_wide_fov_autocull`.

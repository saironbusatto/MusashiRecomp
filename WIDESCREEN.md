# Widescreen support (`feat/widescreen`)

Status as of 2026-06-12. Branch `feat/widescreen` in **both** `psxrecomp`
(framework) and `TombaRecomp` (game opt-in + config). Experimental.

The framework feature is **generic and parameterized**: every game gets the
plain behaviour for free, all knobs default to inert, and `aspect_ratio = "4:3"`
(the default) is a mathematical identity — byte-for-byte the original
presentation. A game opts in via `[video] aspect_ratio` plus an optional
`[widescreen]` block. One build serves both aspects; the launcher's
**Settings → Aspect ratio** toggle selects it at runtime (persisted to
`settings.toml`).

This is the DuckStation/Beetle "widescreen hack" (squash the GTE projection,
present stretched to the wide aspect → wider field of view), but implemented
in our GTE library + GPU so it covers generated code, the interpreter, and
overlay DLLs uniformly — and extended well past what emulators ship with
(per-prim proportion correction, so sprites/HUD are NOT stretched).

---

## What works (committed, user-validated)

| Commit | What |
|---|---|
| `f8a29e8` | `[video] aspect_ratio = "W:H"` → GTE X-squash (`gte_set_display_aspect`, `runtime/src/gte.cpp`) + aspect-aware letterbox in both renderers + window opens at aspect. Config plumbed through `game.toml`, `settings.toml` (load+save), launcher seed (`recompiler/src/config_loader.{h,cpp}`, `runtime/src/main.cpp`, `runtime/src/gpu_gl_renderer.c`). |
| `970670b` | Launcher **Aspect ratio** setting: 4:3 (Native) / 16:9 (Widescreen) / 21:9 (Ultrawide). One build does all. (`runtime/launcher/launcher.cpp`, `assets/launcher.rml`) |
| `cad8c96` | **Per-prim proportion correction.** Recompiler emits `psx_ws_sprite_tag(cpu)` at the entry of each `[widescreen] sprite_tag_funcs` function; runtime records prim ptr (`$a0`) → GTE-projected anchor and re-squashes tagged prims around their own anchor at GP0 execution, so characters/billboards are NOT stretched. Untagged textured rects (SPRT = screen-space HUD/menus) center-squash. (`recompiler/src/code_generator.cpp`, `runtime/src/gpu.c`) |
| `0792086` | **FMV 4:3 pillarbox** (24-bit OR streamed 15-bit MDEC video, detected via `mdec_recently_active`), **menu-2D coherence** (lines/flat-rects/mono-sprites squash on full-2D screens so dialog borders match the SPRT boxes; fades exempt), **HUD edge anchoring** (in-game untagged SPRTs pivot by thirds — outer-third elements keep their wide-screen corner position at native proportions). |
| `5151e5b` | **Authentic 4:3 BIOS boot** — squash held off until game entry PC fires (`fntrace_is_game_started`); Sony/PS logos + shell render 4:3, widescreen engages at game start. |
| TombaRecomp `124e3c9` | Tomba `[widescreen]` block (see config below). Tomba `game.toml` defaults `aspect_ratio = "4:3"`; the dev instance opts into 16:9 via `build-stable/settings.toml`. |

User has visually confirmed: world geometry, characters, HUD, menus, and FMVs
all present at correct (native) proportions on the 16:9 stretch.

---

## Config reference

```toml
[video]
aspect_ratio = "16:9"   # "4:3" (default/identity) | "16:9" | "21:9" | any "W:H"
                        # in [4:3, 32:9]. Wider than 4:3 engages the hack.

[widescreen]            # all optional; inert if absent
sprite_tag_funcs   = ["0x8005E08C"]   # guest addrs of the per-prim helper(s)
                                       # every character/billboard render fn
                                       # calls with the prim ptr in $a0.
sprite_anchor_addr = "0x1F800070"      # scratchpad holding the prim's
                                       # GTE-projected anchor SXY at tag time.
hud_sprt_squash    = true              # center/edge-squash untagged SPRTs
                                       # (screen-space HUD/menus).
```

Tomba's values are Ghidra-evidenced: `0x8005E08C` is the shared per-prim helper
all ~23 character render functions (RTPS cluster `0x800459E0`–`0x8004FB54`) call;
the RTPS preamble stores the anchor SXY to scratchpad `0x1F800070`.

**Changing `sprite_tag_funcs` requires a game regen** (the tag callback is
emitted into the generated C). Everything else is runtime-only.

---

## Known issues / outstanding work

### 1. World-space culling pop-in — ROOT-CAUSED, patch set ready, NOT YET IMPLEMENTED

**This is the top remaining item.** Background scenery and some enemies pop
in/out while still on-screen in 16:9, because the game's per-object draw
classifier tests `objX − camX` against a **4:3-screen-derived window** that the
GTE squash never sees (it's a pre-projection world-space test).

Ghidra investigation (2026-06-12) located the entire classifier family and the
exact patch set. All constants are **code immediates** (no data tables).

- **camera X** (left edge of the 4:3 view) is mirrored each frame to scratchpad
  `*(u16*)0x1F800176` by the camera updater at `0x8002D90C` (adjacent to the
  H=544 re-assert at `0x8002D89C`).
- **Main draw classifier `FUN_80022E44`** (24 call sites — every "register for
  draw" path). Sets per-object draw flag at `obj+1`; the draw walker
  (`0x8004AFAC` / drainer `0x8004B1D8`) skips objects with `obj+1 == 0`
  (`0x8004B230`). Window math:
  ```
  0x80022E78: addiu v0,v0,0x40    ; +64 margin            <- widen
  0x80022E80: sltiu v0,v0,0x1C1   ; pass if < 449 (320+2*64+1)  <- widen
  ```
  With the 4/3 squash showing world `[-53.3, +373.3]`, the effective margin
  collapses to ~11 px → anything wider than ~22 px pops. **Draw-level, not
  despawn.**
- Widen by `E = 320*(4/3 − 1)/2 ≈ 54 (0x36)` symmetrically. Patch sites:

  | Addr | Fn | Current → New |
  |---|---|---|
  | `0x80022E78` | `0x80022E44` draw classifier | `addiu v0,v0,0x40` → `0x76` |
  | `0x80022E80` | " | `sltiu v0,v0,0x1C1` → `0x22D` |
  | `0x80022F94` | `0x80022F6C` predicate(obj) | `addiu v0,v0,0x40` → `0x76` |
  | `0x80022F9C` | " | `sltiu v0,v0,0x1C1` → `0x22D` |
  | `0x80022FE8` | `0x80022FD8` predicate(x,y) | `addiu a0,a0,0x40` → `0x76` |
  | `0x80022FF0` | " | `sltiu a0,a0,0x1C1` → `0x22D` |
  | `0x80023068` | `0x80023020` despawn (160 margin) | `addiu v0,v0,0xA0` → `0xD6` |
  | `0x80023070` | " | `sltiu v0,v0,0x280` → `0x2EC` |

  Param-margin variants (margin in `a1`, width `0x140` hard-coded) at
  `0x80022C08` / `0x80022D3C` / `0x800230BC`: convert the load-delay `nop`
  before the subtract (`0x80022C34` / `0x80022D68` / `0x800230FC`) into
  `addiu a1,a1,0x36` (`0x24A50036`) — hazard-free, inflates both bias and the
  `2*a1` bound. These are called from disc overlays (enemy AI) with no
  boot-EXE xrefs, but patching the shared main-EXE function covers all overlay
  callers.

- **Do NOT touch** `0x800344CC` (world→screen coord conversion).

**Recommended implementation (do NOT hand-patch generated code — Rule 4):**
make this a recompiler feature like `sprite_tag_funcs`. Add a `[widescreen]`
list of `(addr, kind)` cull-widen sites; emit the immediate as a
**runtime-computed term** `imm + psx_ws_x_margin()` where `psx_ws_x_margin()`
returns 0 at 4:3/boot and `~54` (aspect-derived) once engaged. That keeps one
build correct for both aspects and makes 21:9 automatic. The per-instruction
emit site is `CodeGenerator::translate_instruction` (`code_generator.cpp:670`);
the `addiu`/`sltiu` immediate handlers are where the substitution goes,
gated by the configured site set. **Requires a game regen.**

### 2. World-edge exposure (left side)

16:9 can reveal past the authored edge of an area's geometry. Candidate
gameplay clamps that pin an object to `camX+0x134`/`camX+0x136` (the 4:3 right
edge) live at `0x80057D94` / `0x80058580` — these are *gameplay* clamps, not
the cull, and widening them is a separate, lower-priority decision. Some areas
may simply have to accept a little edge reveal.

### 3. HUD edge anchoring is heuristic

In-game untagged SPRTs pivot by thirds (outer third → screen edge, middle →
centre). Works for Tomba's sparse corner HUD; a game with mid-screen HUD
elements could need per-element tuning. The "game vs full-2D screen" mode
detector (`ws_game_mode` in `gpu.c`) keys off "any tagged prim within 2 frames."

### 4. 21:9 untested

The launcher offers 21:9 and the math generalizes, but it has not been
playtested. Wider aspects worsen culling pop-in until issue #1 is fixed.

### 5. `sprite_tag_funcs` discovery is per-game manual

Finding the shared per-prim helper + anchor scratch requires a Ghidra pass per
title. Documented for Tomba; any new title needs the same investigation.

---

## Key files

- `runtime/src/gte.cpp` — `gte_set_display_aspect`, RTPS/RTPT X-squash.
- `runtime/src/gpu.c` — `gpu_ws_configure`, `psx_ws_sprite_tag`, tag table,
  per-prim transforms in the GP0 exec handlers.
- `runtime/src/gpu_gl_renderer.c` — `gl_renderer_set_display_aspect`,
  `letterbox_rect_aspect`, `force_4_3` present paths.
- `runtime/src/main.cpp` — config resolution, `g_ws_engaged` (boot gate),
  `fmv_frame` 4:3 pin, window sizing.
- `runtime/src/mdec.c` — `mdec_recently_active` (FMV detector).
- `recompiler/src/code_generator.cpp` — `psx_ws_sprite_tag` emit at tagged fn
  entry. **This is where the cull-widen emit (issue #1) belongs.**
- `recompiler/src/config_loader.{h,cpp}` — `[widescreen]` parse, `aspect_ratio`.
- `runtime/launcher/launcher.cpp` + `assets/launcher.rml` — aspect toggle.
- `TombaRecomp/tools/capture_game_window.ps1` — window-capture helper for
  visual verification (the VRAM `screenshot_file` shows the pre-stretch frame;
  this shows the post-stretch window).

## Verify / rebuild

Runtime-only changes (everything except `sprite_tag_funcs` / cull-widen) just
need a rebuild:
```
PATH=/c/msys64/mingw64/bin:$PATH
cmake --build F:/Projects/psxrecomp/TombaRecomp/build-stable --target psx-runtime -j16
```
Changes that emit into generated C (`sprite_tag_funcs`, future cull-widen) need
a regen first (see `memory/regen_build_recipe.md`):
```
F:/Projects/psxrecomp/psxrecomp/recompiler/build/psxrecomp-game.exe --config game.toml
```
`build-stable` is RelWithDebInfo + debug tools (TCP port 4470) + launcher ON.
Always `taskkill /F /IM psx-runtime.exe` before relaunching.

# CURRENT_STATE — read this before doing anything

Last updated: 2026-05-17 (Tomba OPTIONS / NEW GAME / save-load milestone)

---

## TL;DR

- **The active test binary is `F:\Projects\TombaRecomp\build\psx-runtime.exe
  --game game.toml`** on debug port 4470.
- Tomba now boots through BIOS, Whoopee Camp FMV, intro FMV, and title.
- User-confirmed working on 2026-05-17: OPTIONS, NEW GAME, SAVE, and LOAD.
- NEW GAME reaches the first in-game area.
- Remaining near-term work is visual correctness: fuzzy title menu glyphs,
  missing BIOS PS-logo glyph, and upcoming in-game rendering fixes.
- The historical Windows "Not Responding" hang is mitigated but should stay
  under observation during longer gameplay soak tests.

---

## What the project layout actually is

There are two repos and several binaries. Get this straight before doing
anything.

```
F:\Projects\psxrecomp-v4\          ← THIS repo
  recompiler\
    src\strict_translator.cpp      ← BIOS-only path (psxrecomp-bios)
    src\full_function_emitter.cpp  ← shared between bios + game paths
    src\code_generator.cpp         ← PS1-EXE / game-only path (psxrecomp-game)
    src\function_analysis.cpp      ← game-only path
    src\main_bios.cpp              ← entry point for psxrecomp-bios.exe
    src\main_psx.cpp               ← entry point for psxrecomp-game.exe / PSXRecomp.exe
  runtime\                         ← the runtime library + psx-runtime.exe target
  generated\                       ← BIOS recomp output (SCPH1001_*.c)
  bios\SCPH1001.BIN                ← the ROM (and SCPH1001.toml audit config)

F:\Projects\TombaRecomp\           ← sibling repo
  generated\SCUS_942.36_*.c        ← Tomba EXE recomp output (separate from BIOS)
  build\psx-runtime.exe            ← THE BINARY THE USER ACTUALLY TESTS
  game.toml                        ← Tomba config
```

`psx-runtime.exe --game game.toml` in TombaRecomp is built from this repo's `runtime/` plus
`TombaRecomp/generated/`. It also links this repo's
`generated/SCPH1001_*.c` for the BIOS half. The cmake macro that does this
is `psxrecomp_v4_add_runtime_target` in this repo's `runtime/CMakeLists.txt`.

`psx-runtime.exe` is a BIOS-only build target in this repo's `runtime/`. It's
the test runner for the discless BIOS shell (memcard / CD player screen).
Nobody has been actively testing it recently; expect it to be subtly broken.

---

## Two recompilers

| Binary | Source set | Input | Output |
|---|---|---|---|
| `psxrecomp-bios.exe` | `strict_translator.cpp` + `full_function_emitter.cpp` + `function_discovery.cpp` + `bios_slice_walker.cpp` + `main_bios.cpp` | flat BIOS ROM at 0xBFC00000 | `generated/SCPH1001_*.c` |
| `psxrecomp-game.exe` (also aliased as `PSXRecomp.exe`) | `code_generator.cpp` + `function_analysis.cpp` + `control_flow.cpp` + `ps1_exe_parser.cpp` + `main_psx.cpp` | PS-X EXE header + body | `<game>_full.c` + `<game>_dispatch.c` |

They share only `mips_decoder.cpp`. **No code path is shared between BIOS
and game translation.** Fixes in one don't apply to the other.

The seeds file (`recompiler/seeds/phase2_ghidra_seeds.json`) is the BIOS
seeds, passed positionally to `psxrecomp-bios.exe --emit-full`. Tomba seeds
live at `TombaRecomp/seeds/`. `bios/SCPH1001.toml` and
`TombaRecomp/game.toml` exist but are currently only consumed by audit tools,
not by the recompilers themselves. (This is one of the things unification
fixes.)

---

## `.happypath/` — what it contains, when to use it

- `.happypath/tomba-runtime.exe` — copy of the May 12 11:55 working binary.
- `.happypath/SCPH1001_full.c.obj` — May 11 23:49 BIOS recomp object file
  (compiled with `-g` debug; .text size 0x371440 = 3.6 MB; **this is the
  reference for "the BIOS output that works"**).
- `.happypath/SCPH1001_dispatch.c.obj` — same era.

**Restore drill**: if a future session breaks tomba-runtime, copy
`.happypath/tomba-runtime.exe` back to `F:\Projects\TombaRecomp\build\` and
stop touching anything until you understand why.

**Sanity check that we haven't drifted**:
```
PATH=/c/msys64/mingw64/bin:$PATH objdump -h .happypath/SCPH1001_full.c.obj
  → .text size MUST be 0x00371440 (debug build with -g, no -O)
```
If a fresh BIOS regen + debug build doesn't produce a matching size, the
generated/ output has changed.

---

## What is currently in working-tree (M / ??)

These were already modified at session start (carried in from prior session):

- `M ISSUES.md` — pre-existing Issue #1-#3; **this session also added Issue
  #4 and Issue #5**.
- `M recompiler/src/code_generator.cpp` — pre-existing mid-function split
  pre-pass cap raised 3 → 16 (Tomba fix; doesn't affect BIOS regen).
- `M runtime/src/debug_server.c` — pre-existing wtrace slot 24/25 ranges
  for Tomba pad-poll + gflag observation.
- `D tools/_codegen_audit.py` — pre-existing rename to `codegen_audit.py`.
- `M tools/gte_audit.py`, `M tools/scan_branch_delay_hazards.py` —
  pre-existing parameterization for audit-config.

This session also added:

- New audit infrastructure: `tools/audit_config.py`,
  `tools/codegen_audit.py`, `tools/codegen_audit_game.py`,
  `tools/verify_audit_schema.py`, `bios/SCPH1001.toml`.
- `docs/audit_inventory.md`.
- An encoding fix to `tools/gte_audit.py` (Unicode `✓` → ASCII `OK`).
- This file (`CURRENT_STATE.md`).
- `UNIFICATION_PLAN.md` (next-step plan).
- `.happypath/` (backup of working binaries).

This session's recompiler-source changes were **reverted** after they
caused confusion; the recompiler/src/ tree is back to clean HEAD.

---

## Why prior sessions got lost (and how to not repeat it)

1. **The handoff said "main menu reachable, Load Game enter/exit works."
   That meant Tomba's main menu, not the BIOS shell.** "Load Game" is the
   in-game save-load menu inside Tomba, not the BIOS shell's load-from-disc
   action. Future handoffs should explicitly write `tomba-runtime: <state>`
   vs `psx-runtime: <state>` so this can't be mistaken.

2. **Rebuilding `psxrecomp-bios.exe` is destructive in two ways**:
   (a) overwrites the binary itself, (b) any subsequent `--emit-full` run
   overwrites `generated/SCPH1001_*.c`. There was no backup of the prior
   working `generated/`. **Now there is** — `.happypath/` has the compiled
   `.obj` files for it.

3. **Debug vs Release matters**. The working `tomba-runtime` is a Debug
   build (`-g`, no `-O`). `psx-runtime`'s CMakeCache had `BUILD_TYPE=Release`
   (`-O3 -DNDEBUG`). The same generated C may behave differently at the
   two optimization levels. If you build the wrong one to validate, you
   will mis-diagnose. **For now, always test the Debug build.** (The
   underlying generated C presumably has undefined-behavior the Release
   optimizer exploits — that's a real bug, but not one we're chasing yet.)

4. **Two runtimes look symmetric but aren't**. `psx-runtime` and
   `tomba-runtime` both link the same `generated/SCPH1001_*.c`, but only
   `tomba-runtime` is being actively exercised. Always check `tomba-runtime`
   first if a "boot" question comes up.

5. **`generated/` is not in git** (commit 802e0fb stopped tracking it).
   The recompiler is the source of truth. **If you regenerate, you cannot
   easily undo.**

---

## The actual broken thing right now

`psx-runtime.exe` does not boot to the BIOS shell. It hangs in the kernel
exception handler / RestoreState loop, with VBlank pending in I_STAT but
never ACK'd. Symptoms (captured in starvation_dump.jsonl earlier):

- Tight loop between `func_00000C80` (kernel exception handler),
  `func_00000EA0` (mfc0 helper), `func_00000F40` (RestoreState),
  `func_1FC40628` (the I_STAT ACK helper for CDROM bit 3 — note: NOT bit 0
  for VBlank), `func_1FC40700`, `func_1FC42BA0` (B0 trampoline at RAM 0xB0
  for `B0:0x17 ReturnFromException`), `func_000005E0` (B0 dispatcher).
- `i_stat = 0x09` (bits 0 = VBlank, 3 = CDROM pending) with `i_mask = 0x4D`
  (bits 0, 2, 3, 6 enabled). CDROM gets ACK'd via `0xFFFFFFF7`; VBlank
  never gets ACK'd.
- The chain handler that should ACK VBlank apparently isn't being invoked,
  or returns 0 (not-handled) so the kernel doesn't ACK.

This is *not* the immediate problem to fix — the unification plan happens
first. But note it here so it doesn't get forgotten. See `ISSUES.md` for
the full open-issue list.

---

## How to verify the happy path is still happy

```bash
# Kill any running runtimes first.
taskkill //F //IM psx-runtime.exe   2>/dev/null
taskkill //F //IM tomba-runtime.exe 2>/dev/null

# Launch the backed-up working binary.
"./.happypath/tomba-runtime.exe"  # debug port 4470

# In a second shell, ping it. If you get a frame number back, it booted.
python tools/_ping_tomba.py
# Expected: {"id":1,"ok":true,"frame":<some-number-going-up>}
```

If the SDL window shows the Tomba main menu, you are at the happy path. Do
**not** rebuild `tomba-runtime` (which would re-link against whatever
`generated/SCPH1001_*.c` currently exists) without first verifying the
generated output matches `.happypath/SCPH1001_full.c.obj`'s `.text` size.

# Building & running Kula World (PSXRecomp) on Windows 11 — AI handoff

**Audience:** an AI coding agent (e.g. Claude Code) running on the user's
Windows 11 machine. Follow this top to bottom. The goal: produce a native
Windows build of Kula World that boots to its menu and plays in 3D, exactly
as it already does on Linux.

---

## 0. Context — what already works and what you are doing

This repo is a fork of Matthew Stanley's **PSXRecomp v4**: a *static
recompiler* that translates the PS1 BIOS (`SCPH1001.BIN`) and a game's MIPS
executable into native **C**, which links into an SDL2 runtime that simulates
the PS1 hardware (CPU state, GPU rasterizer, GTE 3D coprocessor, SPU, CD-ROM).
There is **no interpreter for static code, no HLE, no stubs** — read
`CLAUDE.md` before touching anything; those rules are strict and load-bearing.

**What was done in a prior Linux session (already on this branch):**

1. Ported the runtime's platform layer from Windows-only to cross-platform
   (sockets, `dlopen`, atomics, misc WIN32) — all behind `#ifdef` guards, so
   **Windows still builds unchanged**.
2. Taught the recompiler to accept Kula World's KUSEG-addressed EXE header.
3. Fixed five real bugs that were wedging Kula at boot/first-level-load:
   RAM-mirror folding, per-disc GetID region, CD status bits, the SCPH-1001
   CD-controller version, and an interrupt-latency bug in the dirty-RAM
   interpreter.

Result on Linux: BIOS → title → menu → **select Arcade → 3D level plays, ball
rolls under control.** Your job is to reproduce that on Windows. None of the
above needs redoing — it is all committed. You are just **building** here.

**Branch to use:** `claude/repo-commit-history-1oayou`
(it contains the Linux port + the five fixes + the Kula config + the
`kula-runtime` build target).

---

## 1. Files the USER must supply (copyrighted — NOT in the repo)

These are `.gitignore`d and will not be in a fresh clone. Ask the user to
place them exactly here (paths are relative to the repo root):

| File | Where it goes | What it is |
|---|---|---|
| `SCPH1001.BIN` | `bios/SCPH1001.BIN` | The PS1 BIOS dump (512 KiB). Must be **SCPH1001** (NTSC-U). |
| `Kula World (Europe).bin` | `games/kula/Kula World (Europe).bin` | The user's own disc dump (raw 2352-byte/sector image, ~255 MB). |
| `Kula World (Europe).cue` | `games/kula/Kula World (Europe).cue` | The cue sheet pointing at the `.bin`. |

Do **not** download these. The user has their own legal dump — they provide
them. If they are missing, stop and ask the user for them; do not proceed.

You will additionally **extract one file from the disc** in step 4.

---

## 2. Prerequisites — MSYS2 / mingw-w64 toolchain

PSXRecomp builds with the **MSYS2 mingw64** GCC toolchain (not MSVC — the
generated C and the runtime assume GCC). If MSYS2 is not installed, install it
from https://www.msys2.org/, then open the **"MSYS2 MinGW 64-bit"** shell and:

```bash
pacman -Syu          # update; may ask you to reopen the shell, then run again
pacman -S --needed \
  mingw-w64-x86_64-gcc \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-SDL2 \
  mingw-w64-x86_64-pkgconf \
  git
```

Always work from the **MinGW 64-bit** shell so the toolchain is on PATH. If in
doubt, prefix commands with `PATH=/c/msys64/mingw64/bin:$PATH`.

---

## 3. Clone the fork and check out the branch

```bash
git clone https://github.com/chrisking1981/psxrecomp_kula_world.git
cd psxrecomp_kula_world
git checkout claude/repo-commit-history-1oayou
```

**Submodules are NOT needed.** `lib/RmlUi` and `lib/freetype` are only used by
the optional launcher GUI, which we disable. The recompiler's own deps
(`fmt`, `rabbitizer`, `toml11`, `ELFIO`) and the runtime's `sljit` are vendored
in-tree and already present after clone. Do not run `git submodule update`.

Now drop in the three user-supplied files from step 1.

---

## 4. Extract the game's boot executable from the disc

The recompiler translates the game's **EXE**, read from a file. `game.toml`
expects it at `games/kula/SCES_010.00`. It is the boot binary named in the
disc's `SYSTEM.CNF` (`BOOT = cdrom:\SCES_010.00;1`), living at the disc root.

Extract it to `games/kula/SCES_010.00` (should be **604160 bytes**). Options:

- **Recommended:** `dumpsxiso` from **mkpsxiso**
  (https://github.com/Lameguy64/mkpsxiso). Run it on the `.cue`/`.bin`, then
  copy the extracted `SCES_010.00` into `games/kula/`.
- **Or** any tool that reads an ISO9660 filesystem from a raw Mode-2/2352
  PS1 image (the `.bin` is 2352 bytes/sector, not a plain 2048 ISO).
- **Or** write a small extractor: parse the ISO9660 primary volume descriptor
  (sector 16), walk the root directory to find `SCES_010.00`, and copy its
  extent — remembering each 2352-byte sector carries a 2048-byte user payload
  at a 24-byte offset (Mode 2 Form 1).

Verify: `SCES_010.00` starts with the ASCII magic `PS-X EXE`.

---

## 5. Build the recompiler

The recompiler is a standalone CMake project under `recompiler/`:

```bash
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build recompiler/build -j8
```

This produces `recompiler/build/psxrecomp-bios.exe` and
`recompiler/build/psxrecomp-game.exe`.

---

## 6. Generate the C (BIOS once, game once)

Both outputs are `.gitignore`d, so you must generate them on this machine.

```bash
# 6a. BIOS -> generated/SCPH1001_full.c + SCPH1001_dispatch.c
./recompiler/build/psxrecomp-bios.exe bios/SCPH1001.BIN generated

# 6b. Game -> games/kula/generated/SCES_010.00_full.c + _dispatch.c
#     (reads all paths from games/kula/game.toml, incl. out_dir)
./recompiler/build/psxrecomp-game.exe --config games/kula/game.toml
```

Sanity check afterwards:

```bash
ls -la generated/SCPH1001_full.c games/kula/generated/SCES_010.00_full.c
```

Both must exist and be multi-MB. `games/kula/generated/SCES_010.00_full.c` is
what makes the `kula-runtime` target appear in the runtime build.

---

## 7. Build the runtime (produces the playable binary)

```bash
cmake -S runtime -B runtime/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPSX_LAUNCHER=OFF -DPSX_ENABLE_VULKAN=OFF -DPSX_DEBUG_TOOLS=OFF
cmake --build runtime/build --target kula-runtime -j8
```

Output: `runtime/build/kula-runtime.exe`. The recompiled BIOS **code**, the
recompiled game **code**, and the whole runtime are compiled *into* this one
binary. (`PSX_DEBUG_TOOLS=OFF` gives a lean play build; turn it `ON` only if
you later want the TCP debug server on port 4370.)

---

## 8. Run and play

Run **from the repo root** (so the relative BIOS/disc paths resolve):

```bash
./runtime/build/kula-runtime.exe
```

You should see `psxrecomp: disc region PAL (serial SCES-01000)` in the console,
then a window: Sony logo boot → **KULA WORLD** title → menu. If SDL2.dll is not
found, copy `/c/msys64/mingw64/bin/SDL2.dll` next to `kula-runtime.exe` (or keep
the MinGW `bin` on PATH).

**Keyboard controls** (from `pad_from_keyboard` in `runtime/src/main.cpp`):

| Key | PS1 button |
|---|---|
| Arrow keys | D-pad (roll the ball / move cursor) |
| **X** | Cross — **confirm / select** |
| **S** | Circle — back/cancel |
| Z | Square |
| A | Triangle |
| Enter | Start |
| Right Shift | Select |
| Q / W | L1 / R1 |
| E / R | L2 / R2 |

An SDL game controller also works — just plug it in.

To reach gameplay: at the menu press **Down/Up** to highlight **Arcade**, then
**X** to start a level. Roll the ball with the arrow keys.

---

## 9. Notes, gotchas, and honest status

- **This is not one self-contained file.** The `.exe` has all the *code* baked
  in, but at runtime it still needs, next to it / under the repo root:
  `bios/SCPH1001.BIN`, the disc image (`games/kula/Kula World (Europe).cue` +
  `.bin`), `games/kula/game.toml`, and `SDL2.dll`. It also creates
  `card1.mcd` / `card2.mcd` for saves. Game assets (levels, textures, audio,
  video) live on the disc image and are streamed through the simulated CD-ROM
  — the recomp translates the *program*, not the *data*.
- **BIOS must be SCPH1001.** The CD-controller version fix (`94 09 19 C0`) is
  specific to that BIOS revision. A different BIOS dump may misbehave.
- **Verified working:** boot → menu → navigation → select Arcade → 3D level →
  ball control. **Not yet verified:** completing a level and all mechanics,
  audio output, FMV/MDEC video, in-practice memory-card save/load. Treat those
  as untested, not as known-good. If something breaks there, that is new work —
  diagnose it per the `CLAUDE.md` rules (oracle-verify against Beetle PSX; no
  printf, no stubs), don't paper over it.
- **If the recompiler or a build step fails**, fix the tool/root cause — do not
  route around it (CLAUDE.md §15).
- Building `psx-beetle` (the Beetle PSX oracle) is **optional** and only needed
  for cross-checking against real-hardware behavior, not for playing. Skip it
  unless you are debugging a divergence.

---

### One-shot summary for the agent

> Clone the fork, check out `claude/repo-commit-history-1oayou`, install the
> MSYS2 mingw64 toolchain + SDL2, have the user drop in `SCPH1001.BIN` and their
> Kula `.bin`/`.cue`, extract `SCES_010.00` from the disc, build the recompiler,
> generate the BIOS C and game C, build the `kula-runtime` target with the
> launcher/vulkan/debug options off, then run `kula-runtime.exe` from the repo
> root and confirm the menu appears and Arcade launches a 3D level.

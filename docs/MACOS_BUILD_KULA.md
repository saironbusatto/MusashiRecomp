# Building & running Kula World (PSXRecomp) on macOS — AI/dev handoff

**Audience:** a developer or AI coding agent on macOS (Apple Silicon or Intel).
Goal: a native macOS build of *Kula World* that boots to its menu and plays in
3D, exactly as it already does on Linux and Windows.

This is the macOS counterpart to [`WINDOWS_BUILD_KULA.md`](WINDOWS_BUILD_KULA.md).
The pipeline (clone → user-supplied BIOS+disc → extract the boot EXE → build the
recompiler → generate BIOS C + game C → build `kula-runtime` → run) is identical;
only the toolchain and a couple of platform specifics differ. Read `CLAUDE.md`
first — the no-interpreter / no-HLE / no-stubs rules are load-bearing.

Verified on: **macOS 15.5, Apple Silicon (arm64), AppleClang 17**. The static
path is portable C (an emulator, not a JIT), so Intel Macs should work too.

---

## 1. Files the USER must supply (copyrighted — NOT in the repo)

Same as the Windows doc. Place them relative to the repo root:

| File | Where it goes | What it is |
|---|---|---|
| `SCPH1001.BIN` | `bios/SCPH1001.BIN` | PS1 BIOS dump, 512 KiB, **SCPH1001** (`md5 924e392ed05558ffdb115408c263dccf`). |
| `Kula World (Europe).bin` | `games/kula/Kula World (Europe).bin` | Your own disc dump (raw 2352-byte/sector, ~255 MB). |
| `Kula World (Europe).cue` | `games/kula/Kula World (Europe).cue` | The cue sheet pointing at the `.bin`. |

Do **not** download these — the user provides their own legal dump.

---

## 2. Toolchain (Homebrew instead of MSYS2)

```bash
brew install sdl2 cmake ninja pkg-config
```

Verify SDL2 is visible to pkg-config (the runtime's CMake uses
`pkg_check_modules(SDL2)` on non-MSVC, which Homebrew's `sdl2.pc` satisfies):

```bash
pkg-config --cflags --libs sdl2
# -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 ... -lSDL2
```

If that prints nothing, add Homebrew's pkgconfig dir to `PKG_CONFIG_PATH`
(`export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig"`). AppleClang builds
the very large generated C fine; `brew install gcc` is optional and not needed.

---

## 3. Clone, check out the branch, drop in the files

```bash
git clone https://github.com/chrisking1981/psxrecomp_kula_world.git
cd psxrecomp_kula_world
git checkout claude/repo-commit-history-1oayou
```

Submodules are not needed. Now place the three user-supplied files from §1.

---

## 4. Extract the game's boot EXE from the disc

`game.toml` expects `games/kula/SCES_010.00` (604160 bytes, ASCII magic
`PS-X EXE`). It is the `BOOT = cdrom:\SCES_010.00;1` binary at the disc root.
Use `dumpsxiso` (from mkpsxiso), or any tool that reads an ISO9660 filesystem
from a raw Mode-2/2352 PS1 image, or a tiny extractor that walks the ISO9660
root directory and copies the extent (each 2352-byte sector carries 2048 bytes
of payload at a 24-byte offset — Mode 2 Form 1). LBA 24 on this disc.

```bash
# sanity check
head -c 8 games/kula/SCES_010.00   # -> PS-X EXE
```

---

## 5. Build the recompiler

```bash
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build recompiler/build -j8
```

Produces `recompiler/build/psxrecomp-bios` and `recompiler/build/psxrecomp-game`.

---

## 6. Generate the C (BIOS once, game once)

```bash
# 6a. Full BIOS -> generated/SCPH1001_full.c + SCPH1001_dispatch.c
#     NOTE: use --emit-full with the phase-2 seeds. The bare positional form
#     (psxrecomp-bios <bios> <out>) runs the Phase-1a boot-slice validator, which
#     does NOT emit the full BIOS the runtime links against.
./recompiler/build/psxrecomp-bios bios/SCPH1001.BIN generated \
  --emit-full recompiler/seeds/phase2_ghidra_seeds.json

# 6b. Game -> games/kula/generated/SCES_010.00_full.c + _dispatch.c
./recompiler/build/psxrecomp-game --config games/kula/game.toml
```

Sanity check — both must exist and be multi-MB:

```bash
ls -la generated/SCPH1001_full.c games/kula/generated/SCES_010.00_full.c
```

(One skipped BIOS function, a COP1/FPU opcode `0x11`, is expected — the PS1 has
no FPU. `games/kula/generated/SCES_010.00_full.c` is what makes the
`kula-runtime` target appear in the runtime build.)

---

## 7. Build the runtime (the playable binary)

```bash
cmake -S runtime -B runtime/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPSX_LAUNCHER=OFF -DPSX_ENABLE_VULKAN=OFF -DPSX_DEBUG_TOOLS=OFF
cmake --build runtime/build --target kula-runtime -j8
```

Output: `runtime/build/kula-runtime` (native Mach-O, links SDL2; SDL picks the
Metal backend on Apple Silicon). Set `-DPSX_DEBUG_TOOLS=ON` for the TCP debug
server on port 4370 (`set_input`, `screenshot`, `pad_status`, …).

---

## 8. Run and play

From the repo root (so the relative disc path resolves):

```bash
./runtime/build/kula-runtime
```

Console shows `psxrecomp: disc region PAL (serial SCES-01000)`, then a window:
Sony boot → **KULA WORLD** title + menu (1 Player / 2 Player / Scores /
Options). At the title, **Start** exits the attract demo; select **1 Player**
with **X** to reach a level and roll the ball with the arrow keys.

Keyboard: arrows = D-pad, **X** = Cross/confirm, **S** = Circle, **Enter** =
Start, **Right Shift** = Select. An SDL game controller works too.

---

## 9. macOS platform notes

- **The runtime is POSIX** thanks to the Linux port (BSD sockets, `dlopen`,
  `__atomic`, ucontext fiber backend). Only two small platform gaps needed
  fixing for macOS, both guarded so Windows/Linux stay intact:
  1. `runtime/src/autocompile.c` — added `#include <stdlib.h>` for `getenv`.
     glibc and `windows.h` leak that declaration transitively; the macOS SDK
     does not, so AppleClang errored on the implicit declaration.
  2. `recompiler/src/main_bios.cpp` — the compiler-probe used `>NUL`, which is
     the Windows null device. On POSIX that both misdirects the redirect and
     drops a stray `NUL` file in the cwd; now `#ifdef`'d to `/dev/null`.
- The **ucontext fiber backend** (`runtime/src/psx_fiber.c`) already carried the
  `_XOPEN_SOURCE`/`_DARWIN_C_SOURCE` guards needed for `makecontext`/
  `swapcontext` on macOS — it compiled and ran with no change.
- **No Xvfb needed** — SDL opens a real window.
- **`~/Downloads` is TCC-protected.** If you keep the repo under `~/Downloads`
  and launch via a `.app` bundle or Finder, macOS prompts once for Downloads
  access (needed to read the BIOS/disc). Launching from a terminal that already
  has file access does not prompt.

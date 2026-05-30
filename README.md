<p align="center">
  <img src="docs/assets/psxrecomp-logo.png" alt="PSXRecomp" width="640">
</p>

# PSXRecomp

Generic static recompiler framework for PlayStation 1: MIPS R3000A to C to
native x64.

Background on the original prototype:
[I Built a PS1 Static Recompiler With No Prior Experience (and Claude Code)](https://1379.tech/i-built-a-ps1-static-recompiler-with-no-prior-experience-and-claude-code/)

[![PSXRecomp demo](https://img.youtube.com/vi/CID9oVhgCyY/maxresdefault.jpg)](https://www.youtube.com/watch?v=CID9oVhgCyY)

## What It Is

PSXRecomp translates PS1 MIPS binaries into C, then compiles that C as a
native executable linked against a PS1 hardware runtime. The v4 architecture
recompiles the real SCPH1001 BIOS and runs it as the kernel. There is no HLE
BIOS layer, no stubs, and no general-purpose interpreter fallback for static
code.

PSXRecomp is a framework. Game-specific projects live in their own
repositories and pull this one in to build a game binary. The active end-to-end
target is [TombaRecomp](https://github.com/mstan/TombaRecomp).

## Status

Current milestone as of 2026-05-18:

| Subsystem | State |
|---|---|
| BIOS recompilation (`SCPH1001.BIN`) | Boots and hands off to Tomba |
| Game EXE recompilation | Tomba title, OPTIONS, NEW GAME, save/load, and gameplay reached |
| CD-ROM / MDEC / XA | Tomba FMVs stream and play at the game's 15 fps cadence |
| Memory cards | Tomba save and load verified |
| SIO0 controllers | Digital pad polling plus DualShock config replies used by Tomba |
| GPU | Functional for BIOS boot, FMVs, menus, and first gameplay area |
| Interrupts, COP0, timers | Working for current Tomba path |
| Dirty-RAM support | BIOS/game RAM-installed dispatch paths handled |
| Controller input | Keyboard plus SDL/XInput-style controller mapping via `input.ini` |

Known follow-up work:

- The recent Tomba visual burn-down fixed the BIOS PS logo, title/menu glyph
  seams, dialog/pause panel seams, terrain shading, and shaded textured branch
  rendering observed in the first area.
- SPU coverage is partial; reverb, noise, sweep, and accurate SPU IRQ behavior
  are not complete.
- The historical Windows "Not Responding" hang is mitigated but should stay on
  the watch list until longer in-game soak tests are clean.
- Tomba is the only current game target validated end to end.

For the current game milestone, build and run the sibling TombaRecomp project:

```sh
cd F:/Projects/TombaRecomp
cmake --build build -j16
./build/psx-runtime.exe --game game.toml
```

Running this repository's runtime without `--game` is still useful for
BIOS-only memory card management.

## Release Package

The framework release package is BIOS-only:

1. Download `PSXRecomp-v*-windows-x64.zip` from Releases.
2. Extract it and run `PSXRecomp.exe`.
3. Select your legally obtained `SCPH1001.BIN` BIOS when prompted.

The package does not include a PS1 BIOS, game disc image, generated game code,
or save data. The selected BIOS path is saved next to the executable as
`bios.cfg`; delete that file to pick a different BIOS later.

Game-specific recomp projects, including TombaRecomp, use the same runtime
picker contract but also prompt for a legally obtained game disc image.

## Setup

Builds natively on **Windows (MSVC/MinGW)**, **macOS (Apple Silicon & Intel)**,
and **Linux**. The BIOS thread scheduler uses host fibers — Win32 Fibers on
Windows, `ucontext` on POSIX (`runtime/src/psx_fiber.c`) — so the recompiled
BIOS's cooperative thread switching (the CD-boot handoff in particular) behaves
the same on every platform.

Requirements:

- A C/C++ toolchain: MSVC or MinGW (Windows), Apple Clang (macOS), Clang/GCC (Linux).
- CMake 3.20+. On macOS/Linux also `ninja` and `pkg-config`.
- SDL2: the bundled dev pack on Windows; `brew install sdl2 pkg-config ninja`
  on macOS; `libsdl2-dev` (or distro equivalent) on Linux.
- A legally obtained `SCPH1001.BIN` BIOS dump. Not included.
- For game projects, a legally obtained game disc/EXE dump. Not included.

Build the framework runtime:

```sh
# Windows (MSYS2/MinGW)
cmake -S recompiler -B recompiler/build -G "Unix Makefiles" && cmake --build recompiler/build
cmake -S runtime    -B runtime/build    -G "Unix Makefiles" && cmake --build runtime/build --target psx-runtime

# macOS / Linux (Ninja)
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C recompiler/build
cmake -S runtime    -B runtime/build    -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C runtime/build psx-runtime
```

Game projects generate their own `generated/<serial>_*.c` files and link this
runtime source tree through CMake.

## Keyboard Map

| PSX button | Keyboard |
|---|---|
| D-Pad Up / Down / Left / Right | Arrow keys |
| Cross | X |
| Square | Z |
| Circle | S |
| Triangle | A |
| L1 / R1 | Q / W |
| L2 / R2 | E / R |
| Start | Enter |
| Select | Right Shift |
| Turbo | Tab (hold) |
| Fullscreen | F11 / Alt+Enter / Cmd+F |

## Controller Map

Xbox-style controller defaults are enabled when a controller is connected:

| PSX button | Xbox controller |
|---|---|
| D-Pad Up / Down / Left / Right | D-pad or left stick |
| Cross | A |
| Circle | B |
| Square | X |
| Triangle | Y |
| L1 / R1 | LB / RB |
| L2 / R2 | LT / RT |
| Start | Menu |
| Select | View / Back |

Release builds create/use `input.ini` next to the executable. Edit that file to
change controller device index, deadzone, or button mapping.

## Architecture

The recompiler emits C functions and dispatch tables for BIOS and game code.
The runtime loads the BIOS/game assets into emulated PS1 memory, links the
generated C as native code, and simulates hardware through MMIO handlers for
GPU, DMA, timers, CD-ROM, MDEC, SIO0, memory cards, SPU, GTE, and interrupt
delivery. BIOS A0/B0/C0 vectors go through the recompiled BIOS, not HLE shims.

See `CLAUDE.md`, `PLAN.md`, and `CURRENT_STATE.md` for the development rules
and current project context.

## License

PolyForm Noncommercial 1.0.0. See `LICENSE`.

The PSX BIOS and game disc images remain copyrighted by their respective
owners. This project distributes neither.

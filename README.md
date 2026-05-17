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

Current milestone as of 2026-05-17:

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

Known follow-up work:

- Tomba title-menu glyphs for `NEW GAME / LOAD / OPTIONS` are still fuzzy.
- The BIOS disc-detected screen is still missing the PS logo glyph.
- In-game rendering needs more visual correctness work.
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

## Setup

Requirements:

- Windows 10/11 x64.
- MSYS2 with the `mingw-w64-x86_64` toolchain, CMake 3.20+, and SDL2.
- A legally obtained `SCPH1001.BIN` BIOS dump. Not included.
- For game projects, a legally obtained game disc/EXE dump. Not included.

Build the framework runtime:

```sh
cd F:/Projects/psxrecomp-v4
cmake -S recompiler -B recompiler/build -G "Unix Makefiles"
cmake --build recompiler/build
cmake -S runtime -B runtime/build -G "Unix Makefiles"
cmake --build runtime/build --target psx-runtime
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

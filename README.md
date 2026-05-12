# PSXRecomp

**Generic static recompiler framework for PlayStation 1 — MIPS R3000A → C → native x64.**

*Background on the original prototype: [I Built a PS1 Static Recompiler With No Prior Experience (and Claude Code)](https://1379.tech/i-built-a-ps1-static-recompiler-with-no-prior-experience-and-claude-code/)*

[![PSXRecomp demo](https://img.youtube.com/vi/CID9oVhgCyY/maxresdefault.jpg)](https://www.youtube.com/watch?v=CID9oVhgCyY)

---

## What it is

PSXRecomp translates a PS1 program's MIPS binary into C once, then compiles that C as a native x64 binary linked against a thin runtime. The result runs on PC without an emulator loop.

This is the **v2** revision of PSXRecomp. It is a ground-up rewrite of the original public project (preserved here on the `legacy-2026-05-11` branch).

The defining v2 rule: **no HLE BIOS, no stubs, no interpreter for static code.** v1 implemented PSX kernel calls as C "shims" that emulated what the BIOS *would have done*. v2 recompiles the real SCPH1001 BIOS to C and runs the recompiled BIOS as the kernel. The only interpreted code is the handful of 4-instruction dispatch stubs the BIOS installs into kernel RAM at runtime; everything else is statically recompiled native code.

PSXRecomp is a **framework**, not a single program. Game projects live in their own repositories and pull this one in to build a game-specific binary. The applied example is [TombaRecomp](https://github.com/mstan/TombaRecomp).

---

## Status

This is **heavy WIP**. The repository compiles and produces a working `psx-runtime` that boots the SCPH1001 BIOS through to the memory card management UI. Game-loading is partially implemented and is not part of the recommended use yet.

### What works

| Subsystem | State |
|---|---|
| BIOS recompilation (`SCPH1001.BIN`) | Boots to memory card management screen |
| GPU (basic 2D, BIOS shell UI) | Renders the BIOS shell correctly |
| Memory cards (read/write, files persist on disk) | Working |
| SIO0 (pad input via keyboard) | Working |
| Interrupts, COP0, timers | Working |
| Dirty-RAM interpreter for kernel-installed stubs | Working |

### What does NOT work / is untested

- **CD-ROM / disc-based game loading is untested and almost certainly does not work** in `psx-runtime`. The CDROM controller emulation is partial and the game-EXE-load path has not been validated end-to-end in the framework binary. Loading a disc via `--disc` may stall or panic.
- **MDEC FMV playback** is partial. Audio decode works (XA-ADPCM); video decode is incomplete and runs slowly.
- **SPU** mixes 24 direct ADPCM voices but does not yet model reverb, noise, sweep, or accurate IRQ timing.
- **GPU** does not implement the full PSX command set — many primitives are not yet supported.
- **GTE** (geometry coprocessor) coverage is partial.

Game projects that pull in PSXRecomp will hit these limits long before reaching gameplay. The [TombaRecomp](https://github.com/mstan/TombaRecomp) prototype reaches the main menu with FMVs that play slowly; in-game state is not reachable.

If you want to *use* this repository today, the supported path is the **BIOS-only memory card editor** described under "How to use" below.

---

## Setup

### Requirements

- **Windows 10/11 x64** (Linux/macOS are not currently supported by the runtime build).
- **Sony SCPH1001 BIOS ROM** (`SCPH1001.BIN`, 512 KB) — **you must provide your own legally-obtained dump.** Not included.
- Tools (for building from source): MSYS2 with the `mingw-w64-x86_64` toolchain, CMake ≥ 3.20, SDL2.

### Option A — use the release binary

1. Download the latest `psxrecomp-v2-windows-x64.zip` from [Releases](https://github.com/mstan/psxrecomp/releases).
2. Extract anywhere.
3. Place your `SCPH1001.BIN` in the extracted `bios/` folder. The path must be exactly `bios/SCPH1001.BIN`.
4. Run `psx-runtime.exe`.
5. A blank `card1.mcd` / `card2.mcd` is created in `saves/` on first launch.

### Option B — build from source

```sh
# MSYS2 mingw64 shell:
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2

git clone https://github.com/mstan/psxrecomp.git psxrecomp-v4
cd psxrecomp-v4
mkdir -p bios
cp /path/to/your/SCPH1001.BIN bios/

# Recompile the BIOS to C (one-time; regenerate when the BIOS changes):
cmake -S recompiler -B recompiler/build -G "Unix Makefiles"
cmake --build recompiler/build
./recompiler/build/psxrecomp-bios bios/SCPH1001.BIN generated/

# Build the runtime:
cmake -S runtime -B runtime/build -G "Unix Makefiles"
cmake --build runtime/build --target psx-runtime

# Run:
./runtime/build/psx-runtime.exe
```

---

## How to use

### BIOS shell + memory card management

Launch `psx-runtime.exe` with no arguments (or with `--disc ""`). With no disc inserted, the BIOS goes to its built-in shell — the same shell you get on a real PS1 with no disc in the tray. From there you can:

- View memory card contents on cards 1 and 2.
- Copy save blocks between cards.
- Delete save blocks.
- Format a card.

Memory card files live in `saves/card1.mcd` and `saves/card2.mcd`. They are standard 128 KB raw PS1 memory card images and are interchangeable with the equivalent files from `pcsx-redux`, `duckstation`, `epsxe`, `mednafen`, etc. — drop them into `saves/` to bring saves with you, or copy them out to share.

### Keyboard map (default)

| PSX button | Keyboard |
|---|---|
| D-Pad Up / Down / Left / Right | Arrow keys |
| Cross (✕) | X |
| Square (□) | Z |
| Circle (○) | S |
| Triangle (△) | A |
| L1 / R1 | Q / W |
| L2 / R2 | E / R |
| Start | Enter |
| Select | Right Shift |
| Turbo (fast-forward, no render) | Tab (hold) |

### Command-line options

```
psx-runtime.exe [--bios <path>] [--disc <path>] [--memcard-dir <path>] [--game-root <path>]
```

- `--bios` — override default `bios/SCPH1001.BIN`.
- `--disc` — point at a `.cue`/`.bin` PS1 disc image. **See the warnings above; this path is not reliable yet.**
- `--memcard-dir` — directory holding `card1.mcd` / `card2.mcd`. Default `saves/`.

---

## Architecture (one paragraph)

The recompiler reads the BIOS as a flat ROM at `0xBFC00000` and walks function boundaries from explicit seed addresses, emitting `generated/SCPH1001_full.c` (one C function per recompiled MIPS function) and `generated/SCPH1001_dispatch.c` (function-pointer dispatch table). The runtime loads `SCPH1001.BIN` into emulated PSX memory, links the recompiled BIOS as native C, simulates the hardware (GPU, SPU, CD-ROM, DMA, timers, COP0, SIO0, MDEC, memcards) via per-MMIO C, and routes BIOS calls (`A0` / `B0` / `C0` vectors) through the recompiled functions. The only interpreted code is the handful of 4-instruction dispatch stubs the BIOS writes into RAM at boot. See `CLAUDE.md` for the full architecture rules.

---

## Difference from v1

The original public version of PSXRecomp (preserved on the `legacy-2026-05-11` branch) used:

- **HLE BIOS shims** — handwritten C reimplementations of `OpenEvent`, `StartCard`, `alloc_kernel_memory`, etc., living in `runner/src/bios.c`.
- **A MIPS interpreter** for unknown code paths (`runner/src/interpreter.c`, ~900 LOC).
- **Tomba bundled in the framework repo** as the example game.

v2 (this branch) replaces all of that with real BIOS recompilation and breaks Tomba out into its own repository ([TombaRecomp](https://github.com/mstan/TombaRecomp)). The interpreter is gone except for an ~300-LOC dirty-RAM interpreter dedicated to BIOS-installed runtime stubs, which is a fundamentally different concept from v1's general-purpose fallback interpreter.

The `legacy-2026-05-11` branch is preserved unchanged for anyone who needs the v1 codebase as a reference.

---

## License

PolyForm Noncommercial 1.0.0 — see [`LICENSE`](LICENSE). Non-commercial use (personal, educational, research, hobbyist) is welcome. Commercial use requires a separate license — contact via [1379.tech](https://1379.tech).

The PSX BIOS and any game disc images you supply remain copyrighted by their respective owners. This project distributes neither.

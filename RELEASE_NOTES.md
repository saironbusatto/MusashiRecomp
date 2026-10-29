# Brave Fencer Musashi Recompiled v0.2.0-alpha

Published on GitHub Releases (Windows x64 and Linux x86_64), 2026-10-29.

**Alpha — nobody has finished the game on this build yet.** It boots, plays and saves (memory card save/reload verified), but a crash or softlock later in the game is possible. Keep a memory-card save and a backup of `card1.mcd`.

## What you need
- Your own PS1 BIOS: `SCPH1001.BIN`
- Your own dump of **Brave Fencer Musashi (USA)** (SLUS-00726), as `.cue` + `.bin`

This package contains **no** BIOS, disc image or save data.

## How to run
Extract `MusashiRecomp-v0.2.0-alpha-windows-x64.zip` and run `MusashiRecomp.exe`. Self-contained — no DLLs or runtimes to install.

In the launcher, pick your BIOS and your disc's `.cue` — the launcher verifies the disc. The game starts in 4:3; 16:9 is an experimental toggle in **Settings > Widescreen** (some edge floor tiles can show black).

Settings, memory cards and cache live next to `MusashiRecomp.exe`.

Please report problems with the area/scene where they happened.



---

Older notes below are from the PSXRecomp framework this repository started from.

## PSXRecomp v0.1.0-alpha

Initial public BIOS-only framework release package.

Highlights:

- Packages the PSXRecomp v4 BIOS runtime as `PSXRecomp.exe`.
- Prompts for a user-provided `SCPH1001.BIN` BIOS on first launch.
- Saves the selected BIOS path in `bios.cfg` next to the executable.
- Adds Xbox-style controller support through SDL/XInput.
- Adds configurable controller mapping via `input.ini`.
- Includes the recent Tomba-driven renderer/runtime fixes for the BIOS logo,
  menu text seams, dialog/pause panel seams, terrain shading, and shaded
  textured primitives.

This package does not include a PS1 BIOS, game disc image, generated game code,
save data, or copyrighted Sony/game assets.

# PSXRecomp v0.1.0-alpha

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

# Kula World (SCES-01000) — bring-up findings

Status as of this session: boots on the Linux runtime, past the region
wedge, stalls before the demo level renders. Two bugs found; one fixed.

## Setup

- `games/kula/game.toml` — SCES-01000, load 0x80011000, entry 0x80069F60
  (KUSEG header addresses normalized by the recompiler; see the PS-X EXE
  parser change).
- Recompile: 5297 functions, generated C compiles clean.
- Runtime target `kula-runtime` (runtime/CMakeLists.txt), Linux + Xvfb.
- Oracle: `psx-beetle` (docs/beetle-linux.md) boots the same dump + BIOS
  to demo gameplay — ground truth for every comparison below.

## Bug 1 — RAM mirror not modeled (FIXED)

crt0 parks $sp in the 4th RAM mirror (0x807FFFF8). Guest accesses to the
0x00200000..0x007FFFFF mirrors fell into the open-bus no-op: stack writes
vanished, $ra read back 0, game jumped to address 0 at frame ~891.
Fix: `psx_phys_addr()` folds the 4x mirror before the RAM bounds check
(runtime/src/memory.c). Diagnosed via the null-dispatch capture-freeze on
the fntrace ring (runtime/src/fntrace.c).

## Bug 2 — GetID region hardcoded (FIXED)

GetID's last four response bytes were hardcoded 'SCEI' (NTSC-J). The
kernel CD driver revalidates disc region (ReadTOC + GetID) after the
game's first file load; a PAL disc reporting 'SCEI' threw it into an
endless GetStat/Init loop — CD froze at sector 307 (LBA 318, just before
the HIRO level directory at LBA 319), black screen.
Fix: `cdrom_set_disc_scex()` fed at launch from the disc's SYSTEM.CNF
serial via the existing disc_identity module (PAL->SCEE etc). Verified:
"disc region PAL (serial SCES-01000)"; the game advanced from a hard pin
at frame 890 to 11000+, CD position reached LBA 319.

## Bug 3 — demo level never loads (OPEN)

After the region fix the game runs its main loop (VBLANK ticks,
I_MASK=0x0D, funcs cycling) but never renders: GPU draws frozen at 10054,
no new GPU DMA, no ReadN ever issued, sectors frozen at 307.

The game's OWN CD driver (game code at pc 0x63A14, not the BIOS) loops
Setloc(LBA 16 = ISO PVD) -> Pause, ~every 150 frames (2.5 s), 28 times
and counting, never advancing to read the directory. It reads back
stat=0x02 (motor on, idle) after each Pause and re-loops instead of
proceeding to ReadN.

Interpretation: this is step 1 of the game's "open file" sequence (seek
to PVD, read directory, find level file, read it). It is stuck on step 1
— never reads the PVD — so the demo level file is never found or loaded,
and the attract/demo loop has nothing to draw. The oracle loads this
level (Inca stage visible in demo mode), so the divergence is concrete:
oracle proceeds Setloc -> read; ours re-loops Setloc/Pause.

Next investigation (oracle-diff): capture what psx-beetle's game does at
the same LBA-16 Setloc — same command sequence? what stat / response byte
lets it proceed to the directory read? The 2.5 s retry cadence suggests
the game waits on a condition during those 2.5 s (disc-ready / seek-done /
a flag set by a CD IRQ) that our controller state never satisfies.
Compare the Pause INT2 stat and the game's post-Pause branch condition
(caller of 0x63A14) against the oracle.

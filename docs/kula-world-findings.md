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

## Bug 3 — game's CD-init retry loop never completes (OPEN)

After the region fix the game runs its main loop (VBLANK ticks,
I_MASK=0x0D, funcs cycling) but never renders: GPU draws frozen at 10054,
no new GPU DMA, no ReadN ever issued, sectors frozen at 307.

The game's OWN CD driver (game code, all commands issued from pc 0x63A14,
not the BIOS) runs a slow retry state machine, ~150 frames (2.5 s) between
every command:

  frame 890  : BIOS ReadTOC + GetID (region revalidation, now passes)
  frame 1040 : GetStat  -> stat 0x02
  frame 1190 : Init     -> stat 0x02
  frame 1438 : GetStat  -> stat 0x02
  frame 1586 : Init     -> stat 0x02
  ... GetStat/Init alternating, then transitions to ...
  frame 2971+: Setloc(LBA 16 = ISO PVD) -> Pause, repeating 28x

KEY FINDING: every CD response is CORRECT. GetStat/Init/Pause all return
stat=0x02 (motor on, no error, idle) — exactly what a healthy idle drive
reports — and the INT3/INT2 ACK/COMPLETE pair fires for each. The game
reads the right bytes and STILL re-loops. So this is NOT a wrong-response
-value bug (unlike bug 2).

Because the responses are correct, the game must be waiting on a TIMING or
EVENT condition, not a status value:
  - a per-poll counter / elapsed-time threshold it never reaches, or
  - an interrupt/event delivery (the Init INT2 COMPLETE, or a CD-IRQ-set
    "command done" flag its ISR latches) that is subtly mis-delivered, so
    the main loop's "init done" check never trips and it retries forever.

The 2.5 s cadence is the tell: that is a retry-with-timeout, i.e. the game
issues a command, waits ~2.5 s for a completion signal, times out, retries.
The signal it waits for is what our controller/IRQ path delivers wrong.

### Oracle-diff result (DONE — divergence localized)

Built a CD-command trace hook into psx-beetle (beetle_cdcmd_trace_hook.patch:
a callback in cdc.cpp's command dispatch + `cdrom_cmd_dump` / `cdrom_cmd_reset`
debug commands on the oracle) and captured its boot->demo CD command stream.

The oracle's game reads level data with a clean per-sector pattern:
    Setloc(LBA) -> SeekL -> Setmode(0x80) -> ReadN -> Pause   (x22 ReadN)
plus GetlocL (0x10) position polling (x89). It issues SeekL 18x and ReadN 22x.

Our runtime's game, in the wedge window, issues **NEITHER SeekL NOR ReadN** —
only GetStat/Init/Setloc/Pause. So our game took an error/retry branch the
oracle never enters.

CRUCIAL timeline fact: our runtime DID deliver 307 sectors successfully during
early boot (so ReadN + sector-data delivery WORK). Reading only stops at
frame ~890 — the moment the BIOS does its mid-game region revalidation
(ReadTOC + GetID). After that revalidation the game can never resume reading;
it drops into the GetStat/Init/Setloc/Pause retry loop and stays there.

So the root cause is narrowed to: the BIOS ReadTOC+GetID revalidation (or the
CD-controller state it leaves behind — drive repositioned to lead-in / TOC,
stat/mode reset) prevents the game's CD driver from resuming reads. The region
fix got the revalidation to PASS, but the post-revalidation CD state still
breaks the resume. The oracle does not perform this mid-game revalidation in
the same way, so it reads continuously.

### Sharper localization (DONE) — spurious BIOS disc re-identify

Full oracle command trace confirms: the oracle NEVER issues ReadTOC (0x1E).
Our runtime issues it at frame 890, from the BIOS (Pause@BFC05FE8 ->
ReadTOC@BFC0D760 -> GetID@BFC0D7F0). The sector reads right before are clean
(stat=0x22 motor+read, no error/shell/seek bits), so this is NOT triggered by
a CD error we injected.

Since BOTH runtimes execute the SAME BIOS bytes, our runtime reaching the
BFC0Dxxx disc-identify path while the oracle never does means the BIOS/game
takes a DIFFERENT BRANCH — driven by state, not by the CD hardware sim:
  - most likely a kernel "disc already identified" flag that our earlier boot
    left unset/cleared (so the BIOS re-identifies), where the oracle's is set;
  - or a recompiler-level game-logic divergence that routes our game into a
    BIOS disc service the oracle's run never calls.

After the (now-passing) re-identify, the game's own CD driver cannot resume:
it drops into GetStat/Init then Setloc(16)/Pause and never issues SeekL/ReadN
again — the visible wedge.

Next tool: instruction/branch-level first-divergence hunt between the two
processes (CLAUDE.md's find_first_divergence) to find where the game/BIOS
first branches differently around frame 890. Then either set/preserve the
kernel disc-identify flag correctly, or fix the recompiled branch. The CD
command hook (cdrom_cmd_dump) and the fntrace/wtrace rings on both sides are
the instruments; this is a dedicated next-session task.

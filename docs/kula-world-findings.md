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

Next investigation options:
  1. Live oracle-diff: rebuild psx-beetle WITH a CD-command trace hook
     (mirror the SIO hook in beetle_sio_trace_hook.patch — add a callback
     in cdc.cpp's command dispatch), boot it, and compare the boot->demo
     CD command/IRQ sequence. Does the oracle's game issue the same
     GetStat/Init loop and then proceed? What completion event breaks it
     out that ours doesn't?
  2. Game-side: find the CD-init state machine (caller of the command-issue
     func_800639C8) and its "init complete" branch condition — which RAM
     flag it polls, and which ISR path (game CD IRQ handler) is supposed to
     set it. Then check our INT2-COMPLETE delivery for that command against
     it (timing of present_cdrom_irq vs the game reading the response).
  Suspicion: the second INT (INT2 COMPLETE) for Init/Pause is delivered but
  the game's ISR misses latching its done-flag — a delivery-timing race in
  present_cdrom_irq's generation latch, the same machinery bug 2's SeekL
  fix (c146b95) touched.

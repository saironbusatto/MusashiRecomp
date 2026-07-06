# Kula World (SCES-01000) — bring-up findings

Status: SOLVED — Kula World boots to its title screen + menu (Arcade / Time
Trial / Load Game). Five bugs found and fixed this session; the final one was
the wedge.

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


### Round 3 narrowing (self-stop trap) — the boot flows diverge from power-on

New tool: PSX_CD_TRAP_CMD=<byte> [PSX_CD_TRAP_NTH=<n>] makes the runtime
SIGSTOP itself the instant the Nth matching CD command dispatches (gdb
conditional breakpoints slow the emu ~100x; the self-stop costs nothing).
Attach gdb to the frozen process at leisure.

Findings from trapping ReadTOC #1 and #2 plus a full command-history dump
(command_history ring, 8192 entries, survives the data-event flood):

1. ReadTOC #1 (frame ~545) is the SHELL's disc check (pcs 0xBFC3Fxxx /
   0xBFC40xxx; RAM at 0x56FF0 still holds shell bytes — EXE not loaded yet).
   Legitimate.
2. ReadTOC #2 (frame ~890, the pre-wedge one) is issued from the BIOS BOOT
   FLOW itself — guest stack at the trap holds ONLY BIOS-ROM return
   addresses (Main/StartKernel chain -> 0xBFC0D5A4 -> 0xBFC0D72C), zero game
   frames. It is the boot's post-EXE-load disc re-check, not a game call.
3. OUR full boot command stream (from power-on):
     Test[20], GetStat, GetID, ReadTOC, GetStat, GetID,     <- shell check
     license sectors LBA 4-5, GetID,
     Init, PVD/dir reads, per-sector EXE load ... Pause(f889),
     ReadTOC+GetID (f890, irq-masked, polled)               <- boot re-check
   ORACLE's full stream (same SCPH1001 bytes!):
     Init, Init, GetTN, GetID, Setmode(0), PVD/dir reads,
     Init x3, per-sector EXE load ... -> game reads, NO Test, NO ReadTOC ever.
4. So the divergence does NOT start at frame 890 — the flows differ from the
   FIRST command: the oracle's kernel does an early CdInit (Init,Init,GetTN)
   that our recompiled BIOS never issues, and our shell uses the
   Test+ReadTOC-based check the oracle never runs. Same ROM bytes, different
   path => either a recompiled-BIOS branch divergence in early boot, or a
   hardware-sim input (timer/SPU/CD status read) steering an early branch
   differently.

Next session: first-divergence hunt anchored at the FIRST CD command
(oracle: Init; ours: Test at f539). Instrument the early boot branch that
selects between these kernel CD-init paths. The 0xBFC0Dxxx CD-driver region
is not covered by docs/psx_bios_disasm.txt — needs Ghidra (source of truth
#2) or manual decode of those ROM bytes. The game-side wedge (GetStat/Init
retry loop) is downstream of this boot-flow divergence.


### Round 4 — status-register fix + boot-diff instrumentation status

Fixed vs the oracle source (beetle cdc.cpp Read A==0): our 0x1F801800
status register raised bit 2 (ADPBUSY, "XA-ADPCM playing") permanently —
it must idle at 0 — and never raised bit 7 (BUSYSTS, command written but
response not ready; now mapped to our queued-command window). Verified the
fix does NOT change the boot command stream, so ADPBUSY was not the branch
input steering the early boot; the fix stands on correctness regardless.

Environment note: the "everything got 40x slower" mystery was real and
environmental — leftover wedged instances before a container restart. A
clean baseline runs ~50 fps and reaches the frame-890 re-check in ~13 s.
Rule-15 lesson: measure the baseline before blaming the tool.

Instrumentation gap found while diffing boots: the oracle's cdcmd hook has
no guest PC, and reconstructing command writes from wtrace (index-register
tracking) is unreliable because IRQ handlers interleave index changes.
Also learned: pc 0x63A14 is simply the game driver's generic
write-byte-to-CD-register routine (same pc for all registers, on the
oracle too) — not specific to the retry loop.

NEXT (small, sharp): extend the beetle cdcmd hook with the guest PC
(PS_CPU exposes it), recapture both boot command streams with PCs, and
diff the first divergent command's call site. Then disassemble that
site's branch (capstone is installed) and inspect its inputs on both
sides. All tooling for this exists after this session: self-stop trap,
command-history ring parse, oracle cdcmd dump, oracle wtrace/fntrace.


### Round 5 — ROOT MECHANISM FOUND: kernel flag [0xDFFC] drives the frame-890 ReadTOC

New tool: PSX_FNTRACE_ALL=1 records every dispatch from power-on (fntrace.c),
so the ring holds the earliest boot execution for offline diffs.

Full mechanistic chain of the wedge, traced with the self-stop trap +
fntrace-all + a gdb watchpoint + capstone disassembly of the ROM:

1. The frame-890 ReadTOC is issued by the kernel CD-open function at
   0xBFC0D570 (called from the boot EXE-load path at 0xBFC06C78 / 0xBFC06E98,
   a0=name ptr, a1=1). Its body:
       lw   $t7, -0x2004($t6)   ; t7 = [0xA000DFFC]
       beqz $t7, skip_readtoc   ; flag==0 -> SKIP ReadTOC
       jal  0xBFC0D72C          ; flag!=0 -> ReadTOC (0x1E) then GetID
2. Kernel flag [0xDFFC] (physical 0xDFFC, just below the kernel heap at
   0xE000) is the discriminator:
       ours   : [0xDFFC] = 1 at frame 890  -> ReadTOC -> wedge
       oracle : [0xDFFC] = 0 at the decision -> skip -> reads continue
   Confirmed: at frame 539 (first CD cmd) [0xDFFC]==0 on ours; it flips to 1
   at frame 541.
3. The setter is func_1FC1DF50 (ROM 0xBFC1DF50), called from SHELL code
   (ra=0x80036024). Its store `sw $t6,-0x2004($t7)` with $t6=1 writes
   [0xDFFC]=1 on the a2==7 path; a sibling path writes 0 (0xBFC1DF5C).
4. The shells diverge: our shell runs functions at RAM 0x35000-0x39000
   (reaching the flag-setter at 0x36024, issuing Test[0x20]/ReadTOC), while
   the oracle's shell runs 0x30D00-0x33200 (issuing Init/GetTN) and never
   calls the flag-setter. Same verbatim shell bytes (RAM 0x30D00 == ROM
   0x18D00 confirmed) -> a BRANCH divergence, driven by an input, not code.

So bug 3's root is: our shell takes a different early branch, calls the CD
control func that sets [0xDFFC]=1, and the boot's CD-open then does an extra
ReadTOC that breaks the game's CD reads. The oracle's shell never sets the
flag. REMAINING: the single shell branch (in 0x30000-0x35DC8) that sends our
shell down the 0x36000 path vs the oracle's 0x30000 path, and the hardware/
memory input it reads. That input is the true first divergence.


### Round 6 — version fix removes the spurious ReadTOC (boot now matches oracle)

func_1FC1DF50 (the shell CD-init) sets [0xDFFC]=1 when the CD controller
version byte (Test 0x20, response[0]) is >= 0x95. Our cdrom.c returned the
PSone-era 0x97 0x01 0x10 0xC2 (copied from beetle, which hardcodes it for all
BIOSes) — wrong for the SCPH-1001 BIOS, whose real 1994 sub-CPU reports
0x94 0x09 0x19 0xC0 (high byte 0x94 < 0x95). Fixed cdrom.c Test 0x20 to the
SCPH-1001 value.

RESULT: [0xDFFC] no longer becomes 1 (reads 0x6458, a pointer, like the
oracle's 0x3660); the frame-890 ReadTOC is GONE; the boot command stream now
matches the oracle's (Setloc->SeekL->Setmode->ReadN->Pause x8, no ReadTOC).
This is a genuine correctness fix and closes the boot-flow divergence.

BUT the game still wedges: from ~frame 1039 the GAME's own CD driver (pc
0x63A14, game code) loops GetStat/Init every ~150 frames and never proceeds
to SeekL/ReadN. So the frame-890 ReadTOC was NOT the wedge cause (round 5's
conclusion was wrong) — it was a real but separate inaccuracy. The true wedge
is downstream, in the GAME's CD-init retry, and persists with the boot now
oracle-matched. funcs still cycle (0xf40/0x2dbc) and frames advance, so it is
a spin-wait: the game polls a condition its CD-init never satisfies. NEXT:
diff the game-phase CD command/response sequence (pc 0x63A14) against the
oracle's game, which does the same GetStat/Init but then proceeds to read.


### Round 7 — remaining wedge localized: CD IRQ pending+enabled but never serviced

With the boot fixed, the visible wedge is the game's libcd CD-init retry.
Live state during the loop (gdb):
  i_stat = 0x05  (bit0 VBLANK + bit2 CDROM)   <- CD IRQ IS raised to the CPU
  i_mask = 0x0d  (bits 0,2,3 enabled)         <- CD IRQ IS enabled
  cdrom: irq_flag=0x03 (INT3 unacked), response_count=1, response_read=0
  funcs cycle 0xf40 (ReturnFromException) + 0x2dbc -> CPU DOES take exceptions

So a CD command completed (INT3 + response waiting), the CDROM interrupt is
pending AND unmasked, and the CPU is taking exceptions (VBLANK bit0 gets
cleared each frame) -- yet the CDROM bit (bit2) is NEVER cleared and the
response FIFO is never drained. The game's/libcd's CD interrupt handler in the
BIOS IRQ chain (IntRP) is not servicing the CD IRQ. libcd's CdSync therefore
never completes -> the GetStat/Init retry loop -> black screen.

This is a distinct, deeper issue from everything fixed today (it is a BIOS-
IRQ-chain / libcd-handler / possibly dynamic-handler-install matter, CLAUDE.md
rule 18 territory), not a CD-response-value bug. NEXT: inspect the BIOS IntRP
handler chain and whether libcd's CD handler is installed + run; check our CD
IRQ presentation vs the BIOS root handler's expectations (present_cdrom_irq
generation latch, and whether the handler that should drain the response FIFO
and ack is being dispatched at all).

## Session tally (fixes shipped)
- Bug 1 RAM mirror (memory.c) — FIXED
- Bug 2 GetID region (cdrom.c + main.cpp) — FIXED
- Bug 3a CD status bits ADPBUSY/BUSYSTS (cdrom.c) — FIXED (correctness)
- Bug 3b CD controller version 0x94.. for SCPH-1001 (cdrom.c) — FIXED; removes
  the spurious boot ReadTOC, boot command stream now matches the oracle
- Bug 3c (OPEN) libcd CD-IRQ not serviced -> game CD-init never completes
Tooling built: null-dispatch fntrace freeze, PSX_CD_TRAP_CMD self-stop trap
(runtime + oracle), PSX_FNTRACE_ALL boot trace, oracle cdrom_cmd_dump hook,
Linux port of runtime+oracle, beetle-linux build docs + patches.


### Round 8 — ROOT CAUSE FOUND AND FIXED: dirty-RAM interpreter interrupt latency

The wedge: at ~frame 1039 the game froze in bursts of ~14s, then advanced.
During each freeze:
  SR = 0x40000401 (IEc=1, IM2=1)   -> interrupts enabled + hw line unmasked
  Cause = 0x400 (IP2=1)            -> hardware interrupt pending
  i_stat & i_mask = 0x5            -> VBLANK + CDROM pending and enabled
  in_exception=0, cooldown irrelevant
  total_checks FROZEN              -> psx_check_interrupts is NOT being called
  g_dirty_ram_insns_run advancing ~15M/s, blocks_run ~2M/s

So the MIPS interrupt-take condition (IEc && IM2 && IP2) was fully satisfied,
yet no interrupt was taken because psx_check_interrupts was never called. The
CPU was in the DIRTY-RAM INTERPRETER running libcd's CdSync spin-wait (a guest
loop that lives entirely in dirty RAM), re-entering the interpreter once per
short (~7-insn) block. The interpreter's only interrupt poll was on the
local-dirty-flow path gated by the PER-INVOCATION `(insns_executed & 0xFFF)`
counter, which for short blocks never reaches 0x1000 and, when the loop exits
to dispatch each iteration, is never reached at all. So the spin ran millions
of instructions with no interrupt poll; the CD IRQ that sets libcd's wait-flag
was never serviced; the loop only broke when a rare timeout re-dispatched.

FIX (runtime/src/dirty_ram_interp.c): poll interrupts at the interpreter's
per-invocation entry, gated by a GLOBAL invocation counter (every 64 entries),
with cpu->pc set so the resume is coherent. This is the interpreter analogue
of a static block-leader poll. psx_check_interrupts runs the handler and
returns with registers restored, so the loop safely continues and immediately
sees its wait-flag set.

RESULT: Kula World boots to the title screen and menu (Arcade / Time Trial /
Load Game / Back). Pixels on screen — bug 3 SOLVED.

Why this only bit Kula and not Tomba/MMX6: Kula uses the BIOS/libcd CD path
whose CdSync spin-waits in a dirty-RAM loop; the working titles use their own
CD drivers / loop shapes that hit the interrupt poll differently. This fix is
general — any dirty-RAM guest spin-loop now yields to interrupts.

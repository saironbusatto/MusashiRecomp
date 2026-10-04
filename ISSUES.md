# PSXRecomp v4 — Open Issues

## Issue #1 — Per-card directory load never runs in recomp (Phase 4 blocker)

**Status:** open, root cause narrowed
**Date opened:** 2026-05-03
**Phase:** 4 (BIOS shell + memory-card screen)

### Symptom

Recomp and Beetle both reach the same MEMORY CARD menu screen with the
same correct PSX rendering. **Beetle renders 3 save-block icons in
CARD 1 + 1 face icon in CARD 2; recomp renders zero icons.** The
divergence is the per-card directory load — Beetle reads it from the
on-disk card files, recomp does not.

(Note: the rainbow buttons are the actual PSX BIOS rendering — both
sides have it. Not a bug.)

### Hard data — gate-mode write distribution

Captured side-by-side, both runtimes driven into the memcard screen
with a 240-frame CROSS hold via `press_into_card_view*.py`:

| Mode | Beetle | Recomp | Notes |
|------|--------|--------|-------|
| 0x01 | 122    | 21     | chain reset / BUSY clear |
| 0x02 | **96** | 16     | R-step install (read-step) |
| 0x04 | 2      | 6      | step-3 install |
| 0x08 | 22     | 3      | D-step install (detect) |
| 0x21 | **0**  | **6**  | init+flag — recomp-only path |

R-step (mode 0x02) caller breakdown:

| caller (`ra`) | Beetle | Recomp |
|----|----|----|
| 0xBFC08F28 (icon-load loop) | **40** | **0** |
| 0xBFC09250                  | **16** | **0** |
| 0xBFC08D08                  | 30     | 6     |
| 0xBFC08A0C                  | 8      | 4     |
| 0xBFC08B98                  | 2      | 6     |

**Recomp never reaches the icon-load loop.** That is the divergence.

### What we proved is *not* the cause

- `mem[0x80007520]` ("chain-restart discriminator") is **not** the
  divergence. Both sides have it at 0 in steady state. The earlier
  hypothesis that recomp=0 / Beetle=non-zero at the decision point was
  wrong — both sides clear it in the JAL-delay-slot at PC `0xBFC14CE0`
  (= RAM 0x51E0) every 2 frames before calling `func_00004D6C`.
- The texture corruption on the menu buttons is correct PSX
  rendering, not a bug.
- The per-slot FP table at `mem[0x7528+slot*4]` IS swapped
  dynamically by recomp too (between `0x5B64`, `0x5688`, `0x51F4`) —
  the swap mechanism works. Beetle just performs additional swaps from
  callers `BFC08F28` and `BFC09250` that recomp never reaches.
- `func_00006380` (writes `mem[0x75C0]=1`) and the B-table trampolines
  at `BFC0DA30` / `BFC0DA40` (B0:0x58, B0:0x4D `_card_status`) DO run in
  recomp — `BFC0DA30` 620×, `BFC0DA40` 618×. The trampoline machinery
  works for those.

### Suspected root cause (not yet confirmed)

Recomp **never enters `func_1FC08B3C`** — the per-card directory load
function. The icon-load loop at PC `0xBFC08F1C..0xBFC08F7C` lives
inside this function. The loop does `jal 0xBFC0DA00` (= B0:0x4F
`_card_read`) once per directory entry; Beetle hits this 20× per
session.

Wtrace activity that *appears* to put recomp inside `func_1FC08B3C`
(mode 0x02 writes with `ra=0xBFC08B98`) is reached via the FP-table
swap path, not via the icon loop. The wider function entry at
`label_BFC08B3C` is never logged in `fn_entry_dump` for the entire
[0x1FC08000, 0x1FC0A000) range across 134M captured ring entries.

`func_1FC08B3C` IS in the recomp dispatch table (entry 5449,
`{ 0x1FC08B3Cu, func_1FC08B3C }`) plus 20+ continuation entries. So
the function is recompiled and dispatchable; it's the *caller* that
isn't reaching it.

### Open questions for the next session

1. **What calls `func_1FC08B3C`?** Find the static caller (or callers)
   of ROM `0xBFC08B3C`. Use Ghidra `xrefs 0xBFC08B3C` to enumerate.
2. **Does recomp ever reach `func_000005E0` (B0 dispatcher) and
   `func_000005C4` (A0 dispatcher)?** These live in the recomp
   dispatch table; if recomp doesn't enter them, B-table dispatch is
   broken. Was about to check this when the runtime froze on a
   270M-entry `fn_entry_dump`.
3. **Counter-mismatch hypothesis:** in `func_00004D6C`, the FP-call
   chooses between several outcomes based on `mem[0x7514]`-1.
   `func_00005B64` (the dispatcher FP) returns -1 when counter=0,
   forcing the 0x21 path. Both sides clear `mem[0x7514]` in the same
   place every 2 frames, but Beetle's writes show counter cycles
   1-2-3-4-5-6 inside a single window (132 cycles × 6 calls), while
   recomp's data showed cycles up to 13 with different clear sources.
   The cycle structure differs — needs deeper analysis.

### Diagnostic infrastructure built / fixed this session

- `tools/summarize_gate_modes.py` — tallies gate writes by value
  (mode 0x01/0x02/0x04/0x08/0x21) per caller, recomp + beetle
- `tools/_dump_gate_seq.py` — dump gate writes in seq order with
  `pc/fn/ra/w` fields (extends earlier handoff tool)
- `tools/_dump_addrs.py` — group wtrace by addr, show distinct
  writers per addr
- `tools/_dump_7520_beetle.py` — filter beetle wtrace by addr range
- `tools/_dump_ram_words.py` — RAM dump as 4-byte words, both backends
- `tools/_read_7520.py` — sample mem[0x7520], 0x755A, 0x7568
- `tools/_screenshot_meta.py` — call screenshot/screenshot_file/
  emu_screenshot
- `tools/check_pc_dispatch.py` — group `fn_entry_dump` by `func_addr`
  in a range
- `tools/press_into_card_view_recomp.py` — mirror Beetle's
  press_into_card_view but for recomp
- `tools/arm_gate_trace.py` extended with `[0x7520..0x7524)`,
  `[0x7514..0x7518)`, `[0x7528..0x7538)`, `[0x7258..0x7260)` ranges

### Tooling fix needed (CLAUDE.md Rule 15)

`fn_entry_dump` iterates the entire ring (up to 270M entries) before
applying the addr filter, which **freezes the debug server thread for
seconds-to-minutes** on a populated ring. Both `handle_fn_entry_dump`
and `handle_fn_exit_dump` in `runtime/src/debug_server.c` need the
addr filter applied early, plus a `seq_lo/seq_hi` window cap, so a
filtered query for a small range returns immediately.

### Files with new memories on disk

- `memory/MEMORY.md` (will be updated next session with the
  "0x7520 is NOT the divergence — directory-load function never
  entered" finding)

### Update 2026-05-03 (continued)

**Static call chain to the directory load (via Ghidra xrefs):**

```
????
  -> FUN_bfc24640 / FUN_bfc24a48 / FUN_bfc1cdd8       (3 of the 5 _card_load callers)
       jal 0xB005A9B0  (= jal 0xBFC42BB0 = B0:0x15 _card_load)
  -> FUN_bfc0dac0  (kernel B0:0x15 trampoline body)
       jr $t2 with $t1=0x15 → kernel B0 dispatcher
  -> _card_load (B0:0x15 body in kernel image)
       calls FUN_bfc09914
  -> FUN_bfc09914
       FUN_bfc08b3c(0)   ; load card-1 directory
       FUN_bfc08b3c(0x10); load card-2 directory
  -> FUN_bfc08b3c (the icon-load loop at BFC08F1C..BFC08F7C)
       jal 0xBFC0DA00  (= B0:0x4F _card_read)  per directory entry
```

**On recomp NONE of `FUN_bfc24640`, `FUN_bfc24a48`, `FUN_bfc1cdd8`,
`FUN_bfc09914`, `FUN_bfc08b3c`, `FUN_bfc0c2e8` are ever called.**
The shell never reaches the call sites. Per
`memory/phase4_b0_trampoline_findings.md`: "The shell has a conditional
code path for card init. On DuckStation, the shell enters the card
detection path. In our runtime, this condition fails, so card init is
skipped." This is the same blocker.

**Static caller of FUN_bfc0c2e8 (BootInitMemcards) is NOT static.** It
has zero static xrefs, and the literal `0xBFC0C2E8` does not appear
anywhere in ROM. It's invoked via a kernel function pointer table
populated at runtime.

**Update late 2026-05-03 — actual call path identified via Beetle
fn_trace:**

Beetle's fn_trace ring (with `0x1FC09914` armed as a target) captures:
- `seq=904 JR caller=0x000005D8 ra=0x8003202C` jumping to FUN_bfc09914
- caller PC `0x000005D8` is INSIDE the kernel A0/B0 dispatcher area
  (A0 dispatcher entry is `0x000005C4`, B0 dispatcher is `0x000005E0`)
- `ra=0x8003202C` means the JAL that started this chain was at
  `0x80032024` — which translates to ROM `0xBFC1A024`
- Disassembly at ROM 0xBFC1A024:
  ```
  bfc1a000: li $t8, 1
  bfc1a004: bne $t9, $zero, 0xbfc1a040  ; SKIP if already initialized
  bfc1a008:   sw $t8, -0x5880($at)       ; (delay slot) set init flag
  bfc1a00c: jal 0xb005a960               ; B0:0x4A InitCARD($a0=1)
  bfc1a010:   li $a0, 1
  bfc1a014: jal 0xb005a970               ; B0:0x4B StartCARD($a0=0)
  bfc1a018:   nop
  bfc1a01c: jal 0xb005a980               ; B0:0x5B ChangeClearPad
  bfc1a020:   clear $a0
  bfc1a024: jal 0xb005a990               ; A0:0x70 (_card_load equivalent)
  bfc1a028:   nop
  bfc1a02c: jal 0xb005a9a0               ; A0:0xAD
  bfc1a030:   li $a0, 1
  bfc1a034: li $t0, 1
  bfc1a038: lui $at, 0x8006
  bfc1a03c: sw $t0, 0x66f8($at)          ; mark mem[0x800666F8]=1 (init done)
  bfc1a040: lw $ra, ...
  ```
- Shell calls `jal 0xB005A990` at PC `0xBFC1A024`. The trampoline at
  `0xBFC42990` is **A0:0x70**, which the kernel A-table dispatches to
  `FUN_bfc09914` (the directory loader caller).
- So the actual chain is **A0:0x70**, not B0:0x15. The 5 static
  callers of `FUN_bfc0c2e8` (FUN_bfc24640 etc.) are red herrings —
  they handle COPY/DELETE actions, not boot-time card load.

**Recomp blocker:** the shell function `FUN_bfc1a000` is reached via
indirect dispatch (no static caller in ROM). It is in recomp's
dispatch table (`{0x1FC1A000u, func_1FC1A000}`), but the shell never
dispatches to it. OR — the shell DOES dispatch to it, but the
`bne $t9, $zero, 0xBFC1A040` branch at PC 0xBFC1A004 skips the init
because `$t9 != 0` somehow.

**Confirmed already-known issue.** This matches
`memory/phase4_b0_trampoline_findings.md`: "The shell has a
conditional code path for card init. ... In our runtime, this
condition fails, so card init is skipped and the shell goes directly
to the main menu (state 0x37)."

### Concrete next step

1. Find what calls `FUN_bfc1a000` on Beetle. Arm `0x1FC1A000` in
   Beetle fn_trace and capture the caller. (One JAL produces the
   chain.)
2. On recomp, check whether `func_1FC1A000` is ever entered. If yes:
   capture `$t9` value at entry. If $t9!=0, find what set it.
   If no: trace the upstream call chain to find why the JAL to
   `func_1FC1A000` doesn't fire.
3. Fix the recompiler's emit / generator for whatever instruction or
   branch is producing the divergent state. NEVER hand-patch the
   generated code or hand-call `func_1FC1A000`.

### Update 2026-05-03 (continued, late session)

**Step 1's address range was wrong** — `0x1FC1A000` is a label
INSIDE `func_1FC19FD0`, not the function entry. Querying for the
correct entry `0x1FC19FD0` shows recomp DOES enter:
- `func_1FC19FD0` (BootInitMemcardsShell) at frame 617/2344 with
  `$a0=0x00F000F0` (matches the AND-mask gate at BFC19FDC).
- The full init chain runs: `mem[0x800666F8]` (init done flag) is set
  to `1`, meaning all 5 inner JALs (InitCARD, StartCARD,
  ChangeClearPad, A0:0x70, A0:0xAD) executed.
- `FUN_bfc09914` is entered at frame 617/2344 with `$a0=0`
  (`ra=0x8003202C` — confirming the JAL chain from BootInitMemcardsShell).
- `FUN_bfc08b3c` is entered **12 times** across both slots:
  frame 2344/2350 (initial calls from FUN_bfc09914 with `ra=BFC09934`,
  `BFC0993C`), then frames 2372/2385/2426/2433 (subsequent calls from
  `FUN_bfc089c0` `ra=BFC08A90`).

**Beetle fn_trace confirms the call chain**:
```
JALR @ 0x8004673C → 0x80031FD0 (BootInitMemcardsShell)
  $a0=0x00F000F0, $a1=0x00F000F0, ra=0x80030968
```
The caller PC `0x8004673C` is inside a function at shell-RAM
`0x800466B8` (= ROM `0xBFC2E6B8`), entered via `JAL` from PC
`0x80030960`. The function reads a function-pointer table at
`mem[$a3 + idx*0x88 + 0x34]` and dispatches devices. For card
init, `$t2` = pointer to BootInitMemcardsShell. Recomp ALSO reaches
this function (per `func_1FC2E6B8` having entries) — the BIOS shell
init chain IS running on recomp end-to-end.

**The actual root cause is one level deeper.** `FUN_bfc08b3c`
runs but **bails out in the 15-sector init loop before reaching the
icon-load loop** at PC `0xBFC08F1C..BFC08F7C`:

- `_card_read` (B0:0x4F via trampoline `BFC0DA00`) is called 32 times
  on recomp — `ra=BFC08B98` (sector 0 from FUN_bfc08b3c entry),
  `ra=BFC08D08` (sectors 1+ from the 15-sector loop), `ra=BFC08A0C`
  (FUN_bfc089c0's first read).
- Sector 0 read SUCCEEDS — `mem[0xA000BE48]` shows `'M' 'C' 0 0 …
  0 0 0 0x0E` — the MC magic AND a valid XOR checksum (XOR of bytes
  0..126 = 0x0E = byte 127). The buffer is correctly filled by
  recomp's SIO/card simulation for sector 0.
- The 15-sector loop runs only ~4 iterations per slot before bailing
  to `LAB_bfc08f8c`. Confirmed: `mem[0xA000B9E8..]=0xFFFFFFFF` is
  exactly the bail-clear pattern (`for (i=0; i<0x14; i++) puVar10[i]=
  0xFFFFFFFF`). And `mem[0xA000BA88]=0` is the bail-clear for the
  directory area.
- The validator `FUN_bfc08720` is **never entered** — confirming the
  bail happens BEFORE the checksum check, in the read-gate:

```c
iVar6 = FUN_bfc0da00(slot, sector, buf);
if ((iVar6 != 1) || (iVar6 = FUN_bfc09144(), iVar6 == 0)) {
    goto LAB_bfc08f8c;          // ← bail
}
iVar6 = FUN_bfc08720(buf);      // ← never called
```

Either:
- (a) `_card_read` returns `!= 1` for sector ≥ 1 (i.e. recomp's SIO
  card simulation only completes sector-0 reads correctly), OR
- (b) `FUN_bfc09144` returns 0 — meaning one of the error flags
  `mem[0xA000B9D4..B9E0]` was set instead of the success flag
  `mem[0xA000B9D0]`. The error flags are set by the recomp's
  SIO IRQ / chain coordinator on read failure.

**Concrete next step (next session):** capture `_card_read` return
values via `fn_exit_dump` of `0x1FC0DA00` while card init is happening
(window from frame ~1029-1100), and capture the wtrace lifecycle of
`mem[0xA000B9D0]` and `mem[0xA000B9D4..B9E0]` during the same window
to determine which flag fires for the failing sector.

**Or simpler**: The fix is in `runtime/src/memcard.c` /
`runtime/src/sio.c`. The SIO/card simulation handles sector 0
correctly but fails on sector ≥ 1. The issue is in hardware
simulation, NOT recompiler/codegen. Inspect how the runtime simulates
the multi-sector read protocol.

**This session's findings invalidate the handoff's primary
hypothesis** (`recomp never enters func_1FC1A000`). The shell init
chain IS reaching directory load. The blocker is the runtime's SIO
card simulation failing sector ≥ 1 reads. Per the handoff: "Cycle-paced
SIO works well enough for card detection. Sector-0 detection works."
That hypothesis is correct, but sector ≥ 1 is the new blocker.

**Failed attempt: 67 trampoline seeds (regressed shell main-menu
transition).** I added all 67 missing A0/B0/C0 trampolines (BFC0D8E0..
BFC0D940, BFC16550..BFC16750, BFC42xxx including BFC42BB0=B0:0x15) to
`recompiler/seeds/dispatch_miss_seeds.json`, regenerated, rebuilt.
Result: recomp never advanced past the MAIN MENU — pressing CROSS no
longer transitioned to the memcard menu. Reverted seeds via
`git checkout HEAD -- recompiler/seeds/`. Conclusion: dispatching the
shell trampolines through recompiled C functions broke shell flow.
The existing dirty_ram_dispatch handles them correctly; do not reseed.

**Tool fix landed:** `runtime/src/debug_server.c::fn_dump_parse` now
defaults to a 1M-entry sliding window instead of the full 128M ring,
preventing the multi-second freeze that previously made `fn_entry_dump`
unusable on populated rings.

### Next session — laser focus

The blocker is "find what kernel/shell function call chain *should*
invoke `FUN_bfc0c2e8` (BootInitMemcards) at boot, and why recomp's
shell skips it." Suggested approach:

1. Find the kernel function pointer table that contains
   `0xBFC0C2E8`. The address must appear *somewhere* — maybe in a
   table populated at runtime by `lui+addiu` instructions in another
   function. Search Ghidra for `BFC0` `C2E8` halfword pairs.
2. Look at SystemInit / boot chain (around `0xBFC00150` reset
   vector). Compare DuckStation/Beetle's static fn_trace through boot
   vs recomp's — find where the divergence first occurs.
3. The goal is a single decision point in shell init code where
   recomp takes the "skip card init" branch and Beetle takes the
   "do card init" branch. Compare register state at that branch.

---

## Issue #2 — `fn_entry_dump` / `fn_exit_dump` freeze the debug server on populated rings

**Status:** open
**Date opened:** 2026-05-03

`runtime/src/debug_server.c::handle_fn_entry_dump` and
`handle_fn_exit_dump` walk every entry in the 64K-cap ring (or rather
the entire `[seq_lo, seq_hi)` window, which can be the full ring)
applying the addr filter only AFTER constructing the per-entry buffer
position. With 270M cumulative entries and a 1GB output buffer, a
filtered query for a 0x200-byte addr range still iterates 134M
entries before returning anything. Debug server is single-threaded so
all other commands stall (including `ping`).

**Fix:** apply `addr_lo/hi` filter as the first check inside the
loop before any string formatting; cap `seq_hi - seq_lo` to a
sensible default (e.g. 2M) when the caller doesn't pass an explicit
window.

---

## Issue #3 — BIOS "PlayStation" disc-detected screen is missing the PS logo glyph

**Status:** open
**Date opened:** 2026-05-11
**Phase:** 4 (BIOS shell render)

### Symptom

After the Sony logo, the BIOS shows the second boot screen (the
"PlayStation" disc-detected screen rendered when a disc is present).
On this screen the top region — which on real hardware shows the
stylized PS logo bitmap — is blank. The text below it ("PlayStation",
license string, etc.) renders correctly.

Cosmetic only; boot continues, the disc loads, and Tomba's FMVs start.
Logged here so we don't lose track of it once the FMV cluster is fixed.

### Likely areas

- GPU VRAM upload for the logo bitmap (CopyRectangle / CPU->VRAM
  transfer) may be dropping a region. The other tiles on the same
  screen render, so it isn't a wholesale VRAM/clut wipe.
- Or the logo is sourced from a CD raw-sector / sector-header data
  path that the current `cdrom.c` whole-sector mode doesn't expose
  the way the BIOS expects.

### Concrete next step

Take a Beetle screenshot of the same boot screen as oracle, diff the
VRAM region the logo lives in (Beetle vs recomp at the same frame),
and walk back from the missing pixels via `wtrace` on the source
RAM/VRAM coordinates.

---

## Issue #5 — Mid-function split targets not registered in dispatch table

**Status:** CLOSED 2026-10-03 — does not reproduce; fixed upstream by the
mid-function convergence pre-pass. Audit tooling that would have caught a
regression was broken and has been repaired (see below).
**Date opened:** 2026-05-12
**Phase:** platform (recompiler emit, full_function_emitter path)
**Bug class:** same shape as 2026-05-12 jump-table cross-function fix
(`full_function_emitter.cpp:777`).

### Symptom

When the recompiler emits a branch/jump whose target is a
mid-function address (neither a known block in the current CFG nor a
known function start), it emits `call_by_address(cpu, 0xX); return;`
to defer dispatch to the runtime — but does NOT register `0xX` in the
dispatch table. At runtime, the dispatch binary-search misses, the
target page isn't dirty, so `psx_unknown_dispatch` fires.

### Affected emit sites in `recompiler/src/code_generator.cpp`

| Line | Pattern                                                |
|------|--------------------------------------------------------|
| 970  | conditional-branch taken-arm to mid-func target        |
| 981  | conditional-branch fall-through to mid-func target     |
| 998  | unconditional jump to mid-func target                  |

All three should call `register_cross_function_target(branch_target)`
the same way the jump-table emitter does (line 777 of
`full_function_emitter.cpp`).

### Evidence — Tomba audit, 2 manifestations

From `codegen_audit_game.py --config game.toml`:

```
[2] literal call_by_address targets: 333 unique, 459 sites
    dispatch table size: 1921 entries
    targets in declared code regions: 2
    targets in RAM (dirty-RAM interpreter domain): 331
    in-code targets MISSING from dispatch: 2
      0x800905E4  (1 site)
      0x80090600  (1 site)
```

Both addresses appear in `generated/SCUS_942.36_full.c` with comments
`/* taken: split (mid-func) */` and `/* not taken: split (mid-func) */`
— the recompiler EXPLICITLY knows these are mid-function splits but
fails to register them.

The 331 RAM-domain targets are not bugs — they're runtime-loaded
overlays correctly handled by `dirty_ram_dispatch`.

### Concrete next step

1. Add `register_cross_function_target(branch_target)` at the three
   call sites in `code_generator.cpp` (lines 970, 981, 998).
2. Regen Tomba.
3. Re-run `codegen_audit_game.py` — expect "in-code call_by_address
   misses: 0".
4. Re-run on BIOS to confirm no regression.

### CLOSED 2026-10-03 — measured on BFM; the original symptom is absent

Re-measured on Brave Fencer Musashi (SLUS-00726) rather than Tomba:

```
[2] literal call_by_address targets: 0 unique, 0 sites
    in-code targets MISSING from dispatch: 0
```

**Zero** literal `call_by_address(mid-func)` sites exist in BFM's generated
code, so the three emit sites never fire. The fix that actually landed was
upstream of them: the mid-function **convergence pre-pass**
(`code_generator.cpp`, `MAX_PASSES` raised 3 → 256) now splits every
mid-function target until no new ones appear, so nothing falls through to the
unregistered `call_by_address` path. The `register_cross_function_target`
calls this issue asked for were never needed.

Note the line numbers above (970/981/998) are stale — the sites are now
1549/1567/1594, and they live in the `code_generator.cpp` CPS path, which is a
different emitter from the `full_function_emitter.cpp:849/864` sites that
already did the registration correctly.

### Tooling repaired the same day (Rule 15) — the audit could not detect a regression

Two independent breakages meant this issue's evidence went stale rather than
being disproved:

1. **No config in the tree had an `[audit]` block**, so
   `codegen_audit_game.py` refused to run on *any* game
   (`KeyError: missing [audit] block`). Added one to
   `games/musashi/game.toml` with the code region read from the EXE header
   itself (`t_addr=0x80010000`, `t_size=0x64800` → text 0x80010000-0x80074800,
   file bytes 0x800-0x65000), not assumed.
2. **`RE_DISPATCH_TABLE_ENTRY` matched 0 of 15606 real cases.** It required
   `func_` to follow `case 0xADDR:` immediately, but the emitter emits
   `psx_check_interrupts_dispatch_entry(cpu, A); cpu->pc = 0;` first. Every
   run therefore reported `dispatch table size: 0` and **929 phantom
   tail-call misses**. Regex now matches to end-of-line.

With the tool trustworthy, the real remaining finding is 8 `jal` targets
absent from the static dispatch table — see below.

### Open item — one `jal` target in an unmapped gap (2026-10-03)

Not part of this issue's original claim; recorded because it is a real
dispatch-table gap and would otherwise look like a regression.

8 tail-transfer/jal targets are absent from the static table:

| target | classification |
|---|---|
| `0x800CEDFC`, `0x800CEE74`, `0x800CEEC8`, `0x800D1724`, `0x800D25FC`, `0x801281D8`, `0x8016E918` | inside overlay[1] (0x800CE000-0x80170000) — correctly absent; dirty-RAM interpreter domain |
| `0x800CAE80` | **in a gap** — see below |

Three of the seven overlay ones (`0x800CEDFC`, `0x800CEE74`, `0x800CEEC8`) are
independently confirmed as overlay `dispatch_entry_pcs` in
`runtime/build/overlay_captures.json`.

**`0x800CAE80` is unexplained.** It is the target of a `jal` at 0x8002019C
inside `func_8001FC08` (static text). It lies in the **358 KB gap** between
static text end (0x80074800) and overlay[1] base (0x800CE000), covered by
neither the static text nor any captured overlay. At runtime it would miss
the table, then miss `dirty_ram_dispatch`, then hit `psx_unknown_dispatch`.

A transcription-bug hypothesis was raised and **killed by measurement**: the
emitted address was suspected of being a digit transposition of
`0x800DAE80`. It is not. `get_jump_target` (`control_flow.cpp:48`) implements
the architectural MIPS rule `(PC+4) & 0xF0000000 | idx<<2`, which legitimately
yields 0x800CAE80 for this word (0x0C032BA0) — the load address is *not* the
base for a 26-bit region-relative jump. Five sites in the tree agree
(`mips_decoder.cpp:29`, `function_analysis.cpp:506`,
`full_function_emitter.cpp:277/283`, `strict_translator.cpp:665/679`).

**Unknown, deliberately not guessed:** what lives at 0x800CAE80. Whether that
call is ever executed is not established here. Resolve by tracing the
`func_8001FC08` call site at runtime before treating it as a live defect.

### Audit caveat — "CLEAN" means "no KNOWN defects", not "no defects"

`codegen_audit_game.py` now exits 0 / prints CLEAN on BFM. Do not read that as
proof the build is clean. Two limits, both structural:

1. **Passes [2] and [3] are VACUOUS.** BFM's generated code contains **zero**
   `call_by_address` sites — the emitter produces CPS tail-transfers instead
   (2940 of them). So the pass that was originally written to watch Issue #5's
   exact regression class can no longer fire, for BFM or for any game emitted
   through the CPS path. The tool now prints an explicit VACUOUS note rather
   than letting a structural zero read as "0 bugs".
2. **Pass [4] buckets by declared region.** All 8 remaining tail-call misses
   fall outside `[audit] regions`, so they are reported as expected-absent
   overlay/RAM targets. Seven are confirmed inside captured overlay[1]. The
   eighth, `0x800CAE80`, is bucketed the same way only because it lies outside
   the declared text region — it is in the 358 KB gap between text end
   (0x80074800) and overlay[1] base (0x800CE000), which nothing maps. If the
   declared region is ever widened, that target will move into the DEFECT
   bucket.

So the accurate statement is: **0 known defects, 1 unexplained target, 2 dead
passes.** The exit code is a regression tripwire, not a correctness proof.

---

## Issue #4 — 7 unemitted GTE / COP2 instructions in BIOS Shell code

**Status:** CLOSED 2026-10-03 — false positive. All are emitted; the detection
method was invalid (see below).
**Date opened:** 2026-05-12
**Phase:** 4 (BIOS shell render)
**Likely related to:** Issue #3 (missing PS logo on disc-detected screen)

### Symptom

The Phase B1 `gte_audit` (now generic, code-region-filtered) reports
4 missing `gte_execute` emits and 3 missing LWC2/SWC2 emits in the
BIOS Shell code region. None of the affected PCs appear anywhere in
`generated/SCPH1001_full.c`.

| PC          | Word         | Class          |
|-------------|--------------|----------------|
| 0xBFC34FF8  | 0x4A480012   | GTE MVMVA      |
| 0xBFC3502C  | 0x4A480012   | GTE MVMVA      |
| 0xBFC35064  | 0x4A480012   | GTE MVMVA      |
| 0xBFC350C4  | 0x4A480012   | GTE MVMVA      |
| (3 sites)   | LWC2 / SWC2  | GTE load/store |

These were previously masked by 73 data-region false positives in the
old unfiltered tool. The B1 code-region filter surfaced them.

### Likely cause

MVMVA is matrix-vector-multiply-and-add — the BIOS only uses GTE/3D
math in two narrow places: the boot logo intro and the
disc-detected/PlayStation-logo screen. We boot past both, but
**Issue #3's missing PS-logo glyph is consistent with these
unemitted MVMVA + LWC2/SWC2 sites**: if the recompiler skipped the
3D math that draws the logo, the logo would render as nothing or
garbage but the surrounding text would be fine — exactly Issue #3's
symptom.

Two possible root causes (not yet distinguished):
- **Discovery gap:** the function containing these PCs was never
  identified, so nothing got emitted for it.
- **Emit gap:** the function was discovered but the recompiler
  skipped these specific instructions when translating.

### Concrete next step

Check whether ANY PC near `0xBFC34FF8` appears in
`generated/SCPH1001_full.c`. If neighbors are emitted but the GTE
sites aren't, it's an emit gap (fix in code_generator.cpp). If
neighbors are absent too, it's a discovery gap (fix in function
discovery seeds). Either way, then close Issue #3 alongside.

### CLOSED 2026-10-03 — false positive; every one of these is emitted

The next step above is what produced the false positive, so it is worth
recording why it cannot work. Generated C is a *translation*: it carries a
per-instruction `/* 0xADDR: WORD text */` comment for instructions worth
annotating, but an ordinary instruction's PC does **not** appear as a
token. Grepping the generated file for a PC therefore proves nothing —
neither presence nor absence. The right instrument is `tools/gte_coverage.py`,
which pairs each ROM GTE site with the emit attributed to it:

```
$ python3 tools/gte_coverage.py
=== GTE instructions in BFC34F00-BFC36D00 ===
Total in region: 479
Present in generated: 479
Missing: 0
```

All four MVMVA sites are emitted inside `func_1FC34FA0`, each as
`gte_execute(cpu, 0x0480012)` directly under its own address comment:

```
/* 0xBFC34FF8: 4A480012  gte cmd 0x12 */   gte_execute(cpu, 0x0480012);
/* 0xBFC3502C: 4A480012  gte cmd 0x12 */   gte_execute(cpu, 0x0480012);
/* 0xBFC35064: 4A480012  gte cmd 0x12 */   gte_execute(cpu, 0x0480012);
/* 0xBFC350C4: 4A480012  gte cmd 0x12 */   gte_execute(cpu, 0x0480012);
```

The LWC2/SWC2 class is emitted too, via the `gte_read_data` /
`gte_write_data` helpers that keep register side effects centralized (16
`gte_read_data` in this function) plus a direct `swc2` at 0xBFC35094 — so
"3 missing LWC2/SWC2 emits" was an artifact of counting emits instead of
tracing them.

Corroborating the emit-gap half of the old hypothesis is wrong too: the
containing function was discovered long ago. `func_1FC34FA0` is emitted and
its next emitted sibling is `func_1FC35128`, so these PCs are interior to a
known function rather than sitting in an undiscovered gap.

**Issue #3 is unaffected by this.** The missing PS-logo glyph is still open and
still unexplained; this closure removes only the proposed *cause*. Do not read
it as evidence that the logo bug is fixed.

## Issue #6 — Launcher art has rough cutout edges (memory cards + controllers)

**Status:** open, cosmetic — deferred
**Date opened:** 2026-06-12
**Phase:** Launcher initiative (UI polish)

### Symptom

The launcher dashboard art (disc / controllers / memory cards / logo)
is cropped out of the design mockup and background-knocked-out to
transparent by `tools/crop_launcher_assets.ps1`. The **memory-card and
controller cutouts look rough / jaggy around the edges** — a hard
luminance threshold in the edge flood-fill leaves a 1–2px aliased
fringe (and the anti-aliased boundary pixels that sit just above the
threshold are kept, so the silhouette is stair-stepped). The disc reads
cleaner because its silver rim contrasts more strongly with the dark
background.

### Cause

`FloodTransparent` uses a binary alpha decision (`max(R,G,B) < thresh`
→ alpha 0, else keep). There is no feathering of the boundary band, so
the object silhouette inherits the threshold's hard step. The memory
card and (grey) controller bodies are closer in luminance to the dark
mockup background than the disc is, so the same threshold leaves more
fringe on them.

### Fix options (later)

- Soft alpha ramp across a luminance band (`t_lo..t_hi`) instead of a
  hard cutoff, applied to the flooded boundary pixels.
- Or supersample: crop at 2–4× from the mockup, knock out, then
  downscale with high-quality bicubic so the edge anti-aliases.
- Or hand-mask the four assets once in an image editor (cleanest, but
  manual).
- Best long-term: replace the mockup-derived crops with proper source
  renders (transparent PNGs) — the `decorator: image(...)` pipeline is
  already in place, so it's a drop-in.

### Notes

Tooling: `tools/crop_launcher_assets.ps1` (crop + knockout),
`tools/gen_launcher_assets.ps1` (procedural check/verdict icons),
`tools/shot_launcher.ps1` (screenshot the GL launcher window). Mockup
source: a local mockup PNG (1448×1086). Pure-cosmetic; does not block
Phase 4/5 wiring.

---

## Issue #7 — sljit live execution is unvalidated (pure-live save-load wedge)

**Status:** open, root-caused — fix in progress (branch `feat/sljit-backend`)
**Date opened:** 2026-06-15
**Area:** overlay Tier-2 sljit backend (`runtime/src/overlay_loader.c`,
`overlay_sljit.c`, `code_provider.c`, `overlay_sljit.c` resolution)

### Symptom

With `PSX_OVERLAY_SLJIT_LIVE=1` (the prototype toolchain-less production path),
save/load **wedges the guest**: `reason=atexit, pc=0`, dispatch looping a
BIOS/kernel address (`0xB0`/`0x650`). Reproduces only in live mode — the
dev differential path (`overlay_diff_on`) is clean (0 divergences / 395 shadow
calls). Flagged in commit `81cf21b` as Known Bug #1.

### Root cause (NOT the emitter)

The MIPS→sljit emitter + block-local register allocator are **proven correct**
this session: 66/66 emitter unit, 22/22 `$sp`-balance probe, and byte-identical to
the pre-regalloc emitter across 313 overlay functions
(`runtime/tests/sljit_{emit_test,sp_probe,offline_diff}.c`). The crash is in the
**live-execution wiring**, which bypasses the validation harness:

- `run_shadow_diff` (the device-touch detector + diff gate) is gated on
  `s_diff_mode`. Live mode sets `s_sljit_live`, not `s_diff_mode`, so in live mode
  **the diff never runs.** Therefore:
  - `device_touch` is **never computed** for a live shard → a device-touching
    function runs its shard and **double-executes** its SIO/memory-card/DMA I/O
    against real hardware state, corrupting the kernel byte-handler path → the
    `atexit, pc=0`, kernel-`0xB0/0x650` livelock. *(Known Bug #3 — leading cause.)*
  - Shards run with `diff_passes == 0`, i.e. **completely unvalidated**.
    *(Known Bug #2.)*
- Separately, the verify budget counts **cumulative** (not consecutive) clean
  passes — `diff_passes` never resets on a divergence — so even in dev mode an
  intermittently-wrong shard could reach budget on lucky passes.

The historical "`$sp` off by 0x18 at 0x12E478" crash was a **different, already-fixed
bug** (diff over-scoping the call tree, fixed by per-function isolation in
`81cf21b`) — not this one, and not a codegen bug.

### Fix (SLJIT.md §11 Phase A) — runtime-only, no regen

Collapse blind-live into validated-live: route live-mode shards through the diff
gate (`(s_diff_mode || s_sljit_live)` at the dispatch gate), so `device_touch` is
computed and a shard runs native live ONLY after a clean verify budget; and reset
`diff_passes` to 0 on any divergence (consecutive-clean = "0 divergences"). After
this there is no blind path: device + diverging shards stay on the interpreter.

### Related gaps (same area, tracked in SLJIT.md §11 Phases B–C)

- **gcc>sljit precedence not enforced** (LIFO chain order can put an sljit shard
  ahead of a gcc shard for the same region). → Phase B.
- **Non-dev machine never triggers sljit generation in normal play** (on-miss hook
  gated behind a dead env-only flag; `auto` resolves to gcc on every machine because
  `autocompile_configured()` tests for a config *string*, not a real toolchain). →
  Phase C.

### End-to-end success criteria (per DEBUG.md end-to-end rule)

`PSX_OVERLAY_SLJIT_LIVE=1` (or `auto`→sljit on a toolchain-less box): play through
save-load, FMV, menus, and combat with **0 divergences, 0 wedges**, shards
validated-then-promoted, device functions correctly pinned to the interpreter.
"Diff is clean" or "shard validated" is NOT success until the full save-load loop
closes live.

---

## Issue #8 — HLE boot: game wedges in an event poll loop (BFM)

**Status:** open, two hypotheses eliminated, not root-caused
**Date opened:** 2026-07-27
**Phase:** enhancement tier (HLE is a QoL layer; LLE is unaffected)
**Affects:** Brave Fencer Musashi (SLUS-00726). Not checked on other titles.

### Symptom

With `bios_hle = true` (the framework default) BFM never renders a frame of
content — the window stays blank. With `bios_hle = false` the same build boots
through the Sony and Squaresoft logos to the title screen and attract demo.
`games/musashi/game.toml` pins LLE because of this.

**It is not a boot failure.** The runtime reaches frame 10000+ and ~4.7e9
guest cycles; video keeps advancing. The *guest* is wedged, not the emulator.

### Hard data

Captured live over the debug server, HLE run, ~frame 10000:

| Probe | Value |
|---|---|
| `dispatch_tail` | 1,543,865 dispatches cycling `0x5E0 -> Events -> 0xF40` |
| `event_ring_tail` | 662,892 entries, ENQ/DEQ churn at guest `pc=0x8005DF24` |
| `hle_dump` | backend HLE, boot_skip=1, 2749 calls serviced |
| `evcb_snapshot` | 9 live events, all `status=ENABLED`, entries 0-7 `mode=CALLBACK` with handlers |

`0x5E0` is TableB0Handler and `0xF40` is ReturnFromException
(`docs/psx_bios_disasm.txt`). The loop is therefore: guest issues a B0 event
service, the event system runs, an exception returns, repeat — a poll for an
event that never satisfies. `pc=0x8005DF24` is inside the game's own text
(`0x80010000..0x80074800`) and runs in INTERP mode.

The EvCB table is **healthy**: events opened, ENABLED, callback mode, handlers
populated. It is not a synthesis error in the event table.

### Eliminated (by measurement, not argument)

1. **IRQ mask divergence.** `i_mask` is `0x0000000D` in both LLE and HLE.
   Identical. Not the cause.

2. **Critical-section leak.** IEc (COP0 SR bit 0) is cleared only by
   EnterCriticalSection (syscall 1, `traps.c`) and restored only by
   ExitCriticalSection (syscall 2), and the BIOS boot *does* enter a critical
   section immediately before `Exec()` hands off to the game
   (`docs/psx_bios_disasm.txt:608`), which made an unbalanced pair a plausible
   story. Instrumented it — `critsec_ring`, added in 2fdfbba — and the answer
   is a clean no: balance **-1** (13 enters, 14 exits), every pair in the ring
   correctly matched `0x401 -> 0x400 -> 0x401`, last operation re-enabling IEc.

### Correction to an earlier reading

A single `irq_state` sample showed `IEc=0` with IRQs `0x9` pending and
unmasked, against `IEc=1` on LLE, and that was initially reported as the root
cause. It is not sound evidence: the LLE sample was taken at an idle title
screen and the HLE sample inside a guest that loops through
ReturnFromException, so a one-shot sample will regularly catch the CPU
legitimately inside exception context where `IEc=0` is correct. The
critical-section ring then ruled out the mechanism outright. **Any future pass
must sample IEc/i_stat over time, not once.**

### Next steps

1. Sample `irq_state` repeatedly (~20 samples over 5s) in HLE. If IEc
   oscillates, interrupts *are* being serviced and the question becomes "which
   event never arrives", not "why do no interrupts arrive".
2. If interrupts are being serviced: identify the event the guest polls at
   `0x8005DF24`, then compare LLE vs HLE for who calls `DeliverEvent` for that
   class/spec. Entries 0-7 are class `0xF4000001` (memory card) and
   `0xF0000011`, which points at the card/SIO path.
3. `hle_dump` reports only a total call count. Per-function counters there
   would say immediately which B0 service the 2749 calls went to.

### Why it is parked

LLE reaches the title screen and the attract demo, so HLE is a convenience
(instant boot vs ~40s of real BIOS intro), not a blocker. The reverse
engineering work it was meant to accelerate turns out not to need it: the RAM
hunt boots once and then works against a live instance over the debug server,
so the 40s is a per-session cost, not a per-iteration one.

---

## Issue #9 — Widescreen needs per-game sprite-tag RE, and fails silently without it

**Status:** CLOSED for BFM 2026-08-09 (plan B shipped, user-confirmed on screen).
The general silent-failure guard named below is still NOT implemented.
**Date opened:** 2026-07-27
**Phase:** enhancement tier
**Affects:** any 3D game with no `[widescreen]` block. Found on BFM (SLUS-00726).

### Symptom

Setting `[video] aspect_ratio = "16:9"` appears to work — the boot banner prints
`widescreen 16:9 (GTE X-squash + stretched present; engages at game entry)` and
`gpu_state` reports `configured=1 mode=1 squash=[3,4]` — but the picture on
screen stays 4:3, pillarboxed inside the wide window. **User-confirmed by eye.**

### Root cause

`gpu.c`:

```c
static int ws_game_mode(void) {
    if (ws_full_2d_mode()) return 1;
    return (uint32_t)s_frame_count - ws_last_tag_stamp <= 2;
}
```

`gpu_ws_present_native_43()` returns 1 whenever `!ws_game_mode()`, and squash is
gated behind `ws_active() = ws_configured() && !present_native_43`. So a frame
only goes wide if the **sprite-tag path** stamped `ws_last_tag_stamp` within the
last two frames — and that path only runs for a game that has configured
`[widescreen] sprite_tag_funcs` + `sprite_anchor_addr`, i.e. whose
character-billboard drawing functions have been identified by hand.

With no `[widescreen]` block, `last_tag_frame` stays at its sentinel
(`0xFFFFFC18` observed), every frame classifies as full-2D, and every frame
pillarboxes. The `full_2d = true` escape hatch is not a substitute: it is for
genuine 2D tile games (MMX6) and forces the BG tile-budget reveal cap.

### Why this matters beyond BFM

The failure is silent and actively misleading: banner, `configured`, `mode` and
`squash` all report success while nothing happens. Anyone enabling widescreen on
a new game will believe it worked. Either the banner should state that no
sprite-tag config is present and the setting is therefore inert, or
`aspect_ratio` without a `[widescreen]` block on a non-`full_2d` game should be
rejected at config load.

### What BFM would need

Identify the functions that draw Musashi/NPC billboards and the scratchpad
address holding their projected anchor SXY, then set `sprite_tag_funcs` and
`sprite_anchor_addr`. That is reverse-engineering work of the same class as the
player-struct hunt, not configuration — the Ghidra corpus in
`games/musashi/ghidra/` is the starting point.

Until then `games/musashi/game.toml` deliberately omits `aspect_ratio`, so the
build does not claim a feature it is not delivering.

### Design (2026-08-08) — `BFM_WIDESCREEN_PLAN.md`

Designed, not implemented. One correction to the framing above: the sprite-tag
RE is a requirement of the **squash** strategy, not of widescreen as such. The
gate (`ws_game_mode`, gpu.c:124) blocks native-wide too, but native-wide does no
per-primitive work at all — it needs the tags only as a 3D-frame-vs-2D-screen
detector, which a general GTE-activity test could supply with zero per-game RE.
The squash path was chosen deliberately as the one proven in production on
Tomba; the general route is recorded there as plan B.

**Update 2026-08-08 — discovery done; the squash mechanism does not fit BFM.**
The render funnel was found and the character buffer identified on two
independent tests (see the plan). The blocker is not missing information: the
emitters `FUN_8004ab68` / `FUN_8004b078` / `FUN_8004b614` take the model
command pointer in `$a0`, while `psx_ws_sprite_tag` (gpu.c:804) keys the tag on
`$a0` **being the prim address**. The prim pointer only exists inside their
per-primitive loop. Tagging at function entry would therefore be silently inert
— the Issue #9 failure mode itself. Making squash work needs a recompiler
change (emit the hook at an arbitrary PC, not a function entry); plan B needs a
general GTE-activity frame detector instead. Recommendation is plan B, pending
the user's call.

**CLOSED for BFM 2026-08-09 — plan B was taken and shipped.** The general
GTE-activity 3D-frame detector replaced the sprite-tag path, so BFM renders 16:9
with no per-game RE at all. See `BFM_WIDESCREEN_PLAN.md`.

What is *not* closed is the silent-failure hazard this issue also described: on a
game with no `[widescreen]` block that is not 2D, nothing stamps a frame-mode
signal, so the banner / `configured` / `mode` / `squash` readouts can still
report success while the picture stays 4:3. The shipped detector is general so
this is less likely than it was, but it has not been proven inert and no
config-load guard was added. Carried forward as a known gap, not a BFM defect.


## Issue #10 — Save states taken in overlay code do not restore

**Status:** OPEN, narrowed 2026-10-03 — control run done. The resume PC IS
routed; the failure is in the interpreter's return path at the resumed
function's epilogue, NOT an unroutable resume PC. Not fixed.
**Date opened:** 2026-08-09
**Affects:** Brave Fencer Musashi (SLUS-00726); likely any title using overlays

### Symptom

A state saved while the guest is executing overlay code (resume PC above the
text end, `0x80074800`) terminates the runtime on load:

```
savestate: LOADED slot 4 -> resuming pc=0x8017F8E8
psxrecomp runtime: execution completed, PC=0x00000000
[slice diag] slice_fired=0 dirty_insns=260731063 exit_pc=0x00000000
             dispatchable=0 dirty=0 in_text=0
```

The runtime states outright that the restored PC was not dispatchable, then
falls through to pc=0 and exits.

### The contrast is internal to one build

Two states written minutes apart by the same session, same binary:

| saved | resume PC | region | result |
|---|---|---|---|
| 09:16 | `0x8004B1D8` | static text | loads and runs |
| 09:21 | `0x8017F8E8` | overlay | pc=0 exit |

### Why it matters beyond widescreen

MusaGround needs save states AND overlays together — overlays are where BFM
keeps its gameplay logic (the damage store lives at `0x8014BCB4`). A state that
only survives if the player happens to freeze inside static text is a serious
limitation for the mod, not a corner case.

### CORRECTION 2026-08-09 — the diag zeros are NOT evidence

An earlier version of this issue reasoned from `dispatchable=0 dirty=0
in_text=0` and concluded the restored PC was not dispatchable. **That was a
misreading.** Those are `g_slice_exit_*`, owned by the precise-slicing
mechanism, which is parked and off by default — and the same line reports
`slice_fired=0`, so nothing ever populated them. They are initial values, not a
measurement.

What remains as actual evidence is only this: restoring a state whose resume PC
lies in overlay code is followed by a `PC=0x00000000` exit. That is real and
reproducible. Any mechanism beyond it is currently unsupported.

What IS established about the snapshot, by reading `boot_state.c`: it carries 14
sections (CPU, RAM, SPAD, IRQ, TIMER, CLOCK, GPU, VRAM, SPU, SPURAM, CDROM, DMA,
SIO, DIRTY) and **no overlay section**. Overlays appear only as header integrity
fields (codegen hash, ABI tag, version), which check that the build matches —
not which overlays are mapped. Whether that absence is the cause is exactly what
has not been shown.

### Next step

Run with **`PSX_EXIT_HALT=1`** (main.cpp:3109). It halts and serves at the pc=0
exit instead of shutting down, with the overlays still loaded and the whole
guest state live over TCP — which is the difference between reading a corpse
and questioning a witness. Then ask what the dispatcher actually holds for
0x8017F8E8 at that moment.

Also still outstanding: the control run (load a static-PC state on the same
binary). The attempt made when this was found exited before the load and proved
nothing.

### MEASURED 2026-10-03 — control run done, cause narrowed to mid-function resume

Both gaps above are now closed. The states were on disk the whole time; they
were decoded rather than re-captured (`probes/decode_pst.py`), and all three
carry **identical** integrity fields (`bios_checksum=0xF67ECB99`,
`codegen_hash=0x0F5548CF`, `abi_tag=0x0A`, `codegen_ver=4`) — so they came from
one build and can be compared against each other:

| state | resume PC | region |
|---|---|---|
| slot01 | `0x80042558` | static text (control) |
| slot02 | `0x8004250C` | static text (control) |
| slot04 | `0x8017F8E8` | **overlay** |

Reproduced twice, same process, same binary, control first:

```
savestate: LOADED slot 1 -> resuming pc=0x80042558   -> keeps running
savestate: LOADED slot 4 -> resuming pc=0x8017F8E8
FATAL: top-level dispatch returned PC=0 (abnormal boot exit -- inspect live)
```

So the contrast the issue needed is now real, and the earlier evidence is
vindicated: a state survives iff its resume PC is a static-text address.

**The overlay code IS present after restore.** RAM at `0x8017F8E8` reads
`1b1c050c 21200002 1980043c 44728424 9fdd040c 00000000 14020392 980102ae`,
which decodes to plausible MIPS (`jal 0x8014706C`, `lui`/`addiu`,
`jal 0x8013767C`, `nop`, `lbu`, `sb`) with both `jal` targets inside the overlay
range. `HI`/`LO` after the failed load (`0xFFFFFFFF` / `0xFFFC10BD`) match
slot04's saved values exactly. **The snapshot restored correctly; the bytes are
there.** The failure is not a missing-overlay-section problem.

**What the dispatcher holds for `0x8017F8E8`: nothing usable, but the address
IS routed.** `sljit_try` (a one-shot leaf compile of the function at a live phys
address) discriminates the two cases:

| address | role | `sljit_try` |
|---|---|---|
| `0x800CF854` | registered overlay entry | `compiled:1, insns:4` |
| `0x8017F8E8` | the failing resume PC | `compiled:0` |
| `0x80042558` | working static-text PC | `compiled:0` (not a leaf — expected) |

`overlay_cps_probe` armed at `0x8017F8E8` and then loading slot04 gives the
precise rejection (`count:1`, `ci:-1`, `cands_in_range:0`):
`overlay_find_by_range` finds **no candidate whose declared code ranges contain
`0x17F8E8`**. So the native/CPS continuation re-entry path
(`overlay_loader.c:1714`, which *would* resume mid-function via
`cpu->pc = addr; c->fn(cpu)`) cannot fire — the owning function was never
discovered, so there is no registered entry.

**CORRECTION 2026-10-03 (later the same day) — "the dispatcher holds nothing"
was wrong, and so was the implied cause.** The address *is* routed: the
`dirty_ram_is_dirty` gate at `dirty_ram_interp.c:2128` passes (the snapshot's
DIRTY bitmap has page `0x17F` set — verified by decoding slot04's 64-byte DIRTY
section), so the interpreter executes it. It runs the ~24 bytes from
`0x8017F8E8` and then **fails closed to `PC=0` at `0x8017F900`** — named by the
existing PC=0 tripwire (`freeze_check` → `pczero_count:1`,
`pczero_addr:0x8017F900`).

Decoded from the snapshot's RAM, that address is the **epilogue of the function
the resume landed in**:

```
0x8017F900: lbu   $3, 532($s0)
0x8017F904: sw    $2, 408($s0)
0x8017F908: addiu $3, $v1, 1
0x8017F90C: sb    $3, 532($s0)
0x8017F910: lw    $31, 28($sp)      <- restore ra
0x8017F914: lw    $16, 24($sp)      <- restore s0
0x8017F918: addiu $29, $sp, 32      <- pop frame
0x8017F91C: jr    $31               <- return
```

`$s0 = 0x800DB888`, so every load/store above is valid RAM — no address fault.
`dirty_ram_unsupported` reads all zeros, so the unsupported-opcode path (line
2288) did **not** fire. The remaining candidates are the interpreter's
`jal`/`jalr`/`jr` transfer paths (lines 1215/1232/1386/1403), which set
`cpu->pc = 0` and then try `interp_enter_compiled` / `overlay_loader_call_native`
before falling into the call-contract logic — and where a **sp mismatch at
return starts a bail unwind that publishes `cpu->pc = cpu->gpr[31]`**, i.e. `0`
when `$ra` is 0.

**ROOT CAUSE 2026-10-03 (proven, offline from the snapshot) — the guest executes
`jr $0`.** The epilogue reloads the return address from the stack, and that
stack slot is **zero**:

```
sp = 0x1F800240                       (from the snapshot's CPU section)
0x8017F910: lw    $31, 28($sp)   ->  scratchpad[0x25C] = 0x00000000
0x8017F914: lw    $16, 24($sp)   ->  scratchpad[0x258] = 0x800A6518   (fine)
0x8017F918: addiu $29, $sp, 32
0x8017F91C: jr    $31            ->  jump to 0x00000000
```

`jr $0` **is** the `PC=0` abort, and `pczero_addr=0x8017F900` is exactly the
block containing that sequence. Confirmed end to end:

- `dirty_break_range 0x8017F8E8..0x8017F920` + `savestate load 4` →
  `hits:1, target:0x8017F8E8`, with `ra=0x80056600`, `a0..a3` and
  `sp=0x1F800240` all matching the snapshot. **The interpreter does enter the
  resume block, with correctly restored state.** (An earlier attempt read
  `hits:0` and appeared to disprove this — that run was invalid: the first load
  had already tripped `psx_fatal_halt`, so the guest was frozen and the second
  load was a no-op. Each experiment needs a fresh runtime, armed *before* the
  load.)
- `overlay_find_by_range` returns -1 for `0x17F8E8`, so the native CPS
  continuation path never fires and the interpreter takes over — but it takes
  over *successfully*, and only dies at the `jr`.

**Therefore this is not a dispatch, overlay-loader, interpreter or
call-contract defect.** Every component executed the guest's instruction
faithfully. The guest state itself contains a zero where the return address
should be.

**So the real question is which of these is true, and it is NOT answerable
from this repo alone:**

1. The capture recorded a genuinely degenerate guest state (the real PS1 would
   also `jr $0` there) — in which case the runtime is faithful and the bug is
   that **savestate is offered at unsafe freeze points**; or
2. the SPAD section is captured/restored incorrectly, so the value was non-zero
   live and the snapshot lost it; or
3. the state was captured mid-frame at a point the game never reaches in normal
   play, so resuming there is meaningless.

(2) is cheaply testable and should be ruled out FIRST: write a known pattern to
scratchpad, save, load, read back. (1) requires the Beetle oracle (port 4380) —
per CLAUDE.md §16 that is the only way to settle what real hardware held at
that instant. Do not guess between these.

Also note the control state's resume PC works, so this is specific to states
captured inside overlay code, not to savestates generally.

**A caveat that killed a wrong turn:** `dispatch_check` is useless here. It
answers "was this address recently dispatched" against a bounded ring
(22.7M entries at capture time), and it returns `found:false` for
`0x80042558` too — a PC that demonstrably works. Do not read its zeros as
"undispatchable".

### Fix direction (SUPERSEDED twice — read the ROOT CAUSE section first)

Do **not** implement "snapshot the entry + intra-function offset": with
`ci == -1` there is no registered entry to take an offset against, and the
failure is not in routing at all — the guest's own `jr $0` produces the PC=0.

Do **not** "fix" the interpreter, overlay loader, or call-contract path either:
each was measured executing the guest faithfully. Changing them would break
faithfulness to paper over a state-capture question.

The next step is diagnostic, not a code change: rule out a SPAD
save/restore defect, then settle what real hardware held at that instant via
the Beetle oracle.

### Tooling fixed along the way

- `tools/debug_client.py` had **no binding for `savestate`**, though the server
  implements it (`handle_savestate`). The entire save/load flow was undrivable
  from the sanctioned Rule-3 client — which is why the control run was recorded
  as impossible. Added, plus a usage line.
- `handle_sljit_try` parsed `addr` with hand-rolled `strstr`/`strchr`/`strtoul`,
  which only worked on an **unquoted** JSON number. `debug_client.py` sends hex
  addresses as quoted strings, so the probe answered `"need addr"` for every
  well-formed request — the tool was unreachable exactly as this issue needed
  it. Now parses both shapes; verified quoted-hex works, decimal still works.

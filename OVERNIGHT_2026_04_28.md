# Overnight session — 2026-04-28

Net result: substantial new architectural understanding of the kernel's
memory-card subsystem; **mc_read_done still 0**. No code changes
shipped — investigation only. Running the agent's planned next-steps
list from the handoff revealed the previous memos were partially
incorrect, so the priority of fixes has shifted.

## What was actually proven

1. **Outer coord 0x5000 IS the chain trigger.** Single direct caller is
   FUN_bfc144bc (RAM 0x49BC), the VBLANK chain handler. Disabling it
   via `[0x74BC]=0` poke stops mc_reads dead.
2. **The chain dispatcher 0x4D6C has THREE exit paths:**
   - `v0==0` (more bytes): I_MASK |= 0x80, exit. **Does NOT clear
     [0x755A].** Counter stays incremented.
   - `v0==1` (chain step done): clears [0x755A], resets counter, sets
     [0x7568+slot]=0x01.
   - `v0==-1` (failure): clears [0x755A], sets [0x7568+slot]=0x21.
3. **R1 ALSO sets [0x755A]=1** at 0xBFC15258 (not just D1). Earlier
   memo claimed only D1 sets it — that was wrong.
4. **R-chain DID run 26 times** during the test session; chain trace
   shows counter advancing 1..11 each cycle, all v0=0. No run reached
   counter=12. **R12 and R13 (the data-checksum and 'G'-byte handlers)
   never fire.**
5. **Three chain modes exist:** R (read, entry 0x5688, 13 handlers,
   table 0x6C98), W (write, entry 0x51F4), D (detection, entry 0x5B64,
   4 handlers, table 0x6CCC). Three setup functions at 0xBFC159A0,
   0xBFC15A04, 0xBFC15B00 select which by writing [0x7528+slot*4].
6. **Install handler mode byte:** R-setup writes 2 (RX), W-setup writes
   4 (TX), D-setup writes 8 (no-op). Install handler at 0x641C
   processes byte and increments [0x72F0]; clears [0x75C0]=0 after 128
   bytes.
7. **Both slots' status [0x7568+slot] settle at 0x01 in steady state**
   — outer coord skip-reset path checks bit 0 and exits. Direct byte
   search (`sb zero, 0x7568(?)`) found NO clear sites; whatever clears
   [0x7568+slot] back to 0 (to allow a fresh read) uses non-direct
   addressing. Detection chains keep running because the dispatcher's
   v0=1 path keeps re-arming [0x7568+slot]=0x01 on every cycle.

## Why R-chain stops at R11

Chain dispatcher's v0=0 path doesn't clear [0x755A]. R1 sets it; it
stays set throughout R1..R13 + 128 install handler bytes. On real PSX,
the whole chain finishes in ~4.5ms (well within 16.67ms VBLANK), so
[0x755A] is cleared by R13's v0=1 success path before the next outer
coord call.

In our runtime: the data phase moves too slowly. Install handler
processes ~16 bytes per cycle, then VBLANK fires (or VBLANK_DEFER_STALE
times out at 500000 dispatch ticks ≈ 125ms with no SIO progress) →
outer coord runs → sees [0x755A]==1 → abort cleanup → chain reset.

## Why _card_read isn't retriggered

After the first failed attempt, [0x7568+slot]=0x01 settles; outer
coord skip-reset exits without re-triggering chain. txn_dump for the
recent test window shows ALL 51 transactions are 4-byte detection
(`[0x81, 0x52, 0x00, 0x00]` / `[0xFF, 0x00, 0x5A, 0x5D]`) ending in
`abort_reselect`. R-chain transactions are older (pre-mc_seq 43689).

The BIOS shell isn't re-issuing _card_read on subsequent X presses.
Either the shell is stuck waiting for a callback that never fired, or
X press doesn't normally trigger another read once you're past the
initial directory load.

## Files added/changed this session

- **NO source code changes.** Pure investigation.
- `probes/overnight/` — 11 JSON dumps + 5 Python analyzers + stdout/
  stderr from the test run.
- New memory:
  - `phase4_chain_dispatcher_complete_map.md` — full architectural map
  - `phase4_real_blocker_2026_04_28.md` — corrected blocker analysis
- Memory NOT yet superseded but should be: `phase4_chain_resets_after_16_bytes.md`
  (the underlying observation is right; the "D1 unconditionally"
  framing is incorrect — R1 also sets [0x755A]).

## Concrete next steps (priority)

1. **Find the `[0x7568+slot]=0` clearer.** It's the gate for fresh
   reads. Likely uses `sw zero, X(base)` where base+X = 0x7568+slot.
   Search for `addiu reg, reg, 0x7568` then trace stores via reg.
   Once found, you know who triggers fresh _card_read calls.
2. **Confirm R-chain's data-phase timing.** Add a wtrace slot for
   [0x72F0] (data byte counter) and watch how high it gets per cycle
   under different timing settings. Currently appears to top out at
   ~16 of 128 needed.
3. **Try lowering SIO_IRQ_DELAY_CARD from 8 to 4** in `runtime/src/sio.c`.
   Risk: breaks the BIOS pad/card detection write-clear-check sequence.
   But install handler currently does only ~4 SIO accesses post-TX, so
   countdown=8 leaves IRQ pending after handler returns. Lower delay =
   IRQ fires inside handler = next byte processed immediately =
   faster data phase.
4. **Check user-level _card_read in B0 vector** to see what it does
   on entry. Specifically: does it clear [0x7568+slot]=0? If yes, why
   isn't it being called on X press? Check what calls _card_read.
5. **Examine kernel callback 0xB0006B30** (RAM 0x6B30 = ROM 0xBFC16630).
   Called by chain dispatcher v0=1 success path with [0x7520]==0.
   Might be the "user notify" hook that BIOS shell's _card_read
   blocks on.

## Hard rules respected

- No printf debugging.
- No generated/SCPH1001*.c modifications.
- No HLE shims.
- No stubs.
- One runtime instance at a time (always taskkilled before launch).
- Build via mingw64.
- No fprintf added to runtime.
- No game ISO/EXE loaded.

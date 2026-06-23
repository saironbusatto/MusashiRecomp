# Tomba 2 splash→attract — investigation plan (Recomp GPT, 2026-06-22)

Frame-1997 is FIXED. Next: the recomp softlocks on the Whoopee-Camp splash; the Beetle
oracle reaches the attract. The splash state machine never advances. CD ruled out (attract
data loads correctly). This is a first-divergence in the scene-transition logic.

## Strategy — REVERSE WRITER HUNT (not a PC diff)
Both recomp and oracle reach the same idle PC `0x80050CE8`, so a raw PC-stream diff buries
the signal (code reconverges, state diverged). Instead: make the ORACLE stop itself at the
successful transition and dump the write history *before* it (the recomp never reaches that
trigger). Find the FIRST oracle write/branch that selects "advance" while the recomp selects
"stay in state 0." It may NOT be the state byte itself — could be a nearby flag the state
machine later consumes.

## Likely gate (ranked by ChatGPT, given our evidence)
1. **GPU / DrawSync / render-completion flag (most likely).** State-0 path is render-heavy
   (`jal 0x8008179C / 0x800815D0 / 0x80081560` + double-buffer toggle + wait-2-VBlank + loop).
   VBlank pacing works + CD loaded, but a DrawSync / GPU-done / fade-complete predicate never
   becomes true → state stays 0. Symptom matches exactly.
2. **A countdown/fade variable, NOT the state byte** (`if(++fade>=limit) state=1` /
   `if(palette_fade_done) state++`). → trace the WHOLE scratchpad + nearby globals.
3. **CD load-COMPLETE EVENT** (CdSync/DeliverEvent/TestEvent/WaitEvent/callback flag): bytes
   loaded but the completion event/flag differs. (Data movement already ruled out; the EVENT is not.)
4. Less likely: input timeout; thread/TCB scheduling (callback writes the flag while the loop polls).
5. Unlikely: MDEC/FMV (only chase if the oracle pre-trigger trace shows MDEC/DMA0 activity).

## Concrete capture plan
**Step A — transition trigger (both runtimes), fire on ANY of:**
- PC enters attract code `0x80041800..0x80052000`
- PC leaves splash loop `0x80050C40..0x80051FFF`
- write to `0x1F8001A4` changes it, OR nearby cluster `0x1F8001A0` changes
(Do NOT require the state byte as the only trigger — the real flag may be elsewhere.)

**Step B — trace ALL scratchpad writes in Beetle** `0x1F800000..0x1F8003FF`, plus clusters
`0x800E8080..0x800E80C0` (VBlank gate counter), `0x1F800180..0x1F800260` (splash state
cluster), `0x80041000..0x80052000` (attract overlay). Per write record: seq, cycle/frame/
vblank, pc, ra, opcode, width, addr, old, new, state byte, threshold, counter, irq stat/mask,
BIOS event/callback id, cd state, gpu dma/status/drawsync.

**Step C — outer-loop snapshots** at each `0x80050CE8` / `0x80050C40` hit: iter, pc, ra, state,
threshold, vblank_counter, hash(scratchpad), hash(0x800E8000..9000), hash(attract code),
irq, cd_state, gpu_status, drawsync, pad. Compare Beetle vs recomp BY OUTER-LOOP ITERATION,
not global frame. Expect: equal for iters 0..N, then Beetle's hash changes at N+1 and advances
at N+2; recomp missing the same write.

**Step D — identify the writer PC** of the first advance-flag change + the predicate loads
immediately before it. Decode that PC's live RAM. Branch-trace around it. Run recomp to the
same iteration and compare the loaded values. Then classify:
- GPU busy/drawsync wrong → GPU event/status bug
- BIOS event flag → DeliverEvent/TestEvent/WaitEvent bug
- CD sync/callback flag → CD event-completion bug (not DMA/data)
- pad state → input bug
- timer/frame counter → root-counter/VBlank-accounting bug
- stale local → dirty-RAM interp / block-return / store-width / ENDIAN bug

## TWO TRAPS TO CHECK FIRST (cheap, do before the hunt)
1. **Committed-PC in the trace.** This is dirty-interp overlay code; verify the write/branch
   trace records the ACTUAL committed PC (after the frame-1997 committed-PC fix) — else the
   writer/caller attribution lies.
2. **Byte/halfword scratchpad endian.** A scratchpad BYTE state (`0x1F8001A4`/`0x1F80019C`) is
   exactly where a store-width / endian / logging mismatch makes the trace lie. Verify sb/sh
   into scratchpad are endian-correct in BOTH tracing and execution.

## NOTE / correction to verify
Re-derive the real state byte from the live disasm: `0x80050D00: lbu v1,412(s5)` with
s5=0x1F800000 ⇒ state byte = **`0x1F80019C`** (offset 0x19C), not 0x1F8001A4 as first sampled.
Confirm which byte the branch-out-of-state-0 actually consumes before instrumenting.

## First run order (ChatGPT)
1. Beetle only: arm scratchpad + `0x800E8080..0x800E80C0` write rings; run to attract; dump last ~8k writes/branches.
2. Find first write changing the state byte / nearby flag / a RAM flag read by the state-0 exit branch.
3. Decode the writer's live RAM code (read_ram).
4. Branch-trace the writer/caller block (source PC, condition regs, taken/not, memory loads).
5. Run recomp to the same splash outer-loop iteration; compare those exact loaded values → classify the subsystem.

Highest-value first artifact: the **Beetle pre-attract write ring ending at the first
transition out of state 0** + the writer PC + the predicate loads right before it.

---

## EXECUTION PROGRESS (2026-06-22 sess2, partial)
- Splash loop is at `0x80050C40+`; state machine reads byte `[0x1F80019C]` (`lbu v1,412(s5)`,
  s5=0x1F800000 @ `0x80050D00`): state 0 → render handler `0x80050D44` which just renders and
  `j 0x80050C6C` (LOOPS, never advances); state 1 → loop; state 2 → handler `0x80050D98` which
  FADES + `sb s2,0x19C(s5)` sets state=1; state 3 → `0x80050DD8`. **The state-0 handler never
  advances the state** → the 0→2 advance must come from an EXTERNAL writer (callback/event).
- Scanned ALL loaded code `0x80040000..0x80092000` for stores to offset `0x19C`: only TWO —
  `0x80050A30 sb zero` (init→0) and `0x80050DA8 sb s2` (state-2 handler →1). **NOTHING writes
  the state byte to 2.** So the 0→2 writer uses different addressing (base≠0x1F800000 / offset
  ≠0x19C) OR is in code not matched by the scan — i.e. it is event/callback-driven (ChatGPT
  hypothesis #1/#3/#4), reachable only at the transition.
- Confirmed `0x80050CE4/CE8` is a COMMON VSync-wait loop: the Beetle oracle (visually in the
  ATTRACT scene) ALSO samples PC=`0x80050CE4`. So PC-sampling cannot distinguish scenes — the
  scene state is in RAM/scratchpad (recomp cluster `0x1F800198..1A4` = all ZERO; oracle = real
  data). Apples-to-apples requires catching the oracle AT the splash, not at attract.
- **DECISIVE NEXT STEP:** reverse writer hunt on the oracle — restart Beetle with a write
  watch/trace on `[0x1F80019C]` (+ cluster `0x1F800180..0x1F8001C0`) armed from boot; catch the
  write that sets it 0→non-zero while PC is in the splash range `0x80050xxx`; that writer PC +
  its predicate loads are the answer. Then check whether the recomp ever executes that PC and
  why the predicate differs. (TCP debug works fine; the browser flakiness is unrelated.)
- Recomp runs STABLY stuck on the splash to frame 78k+ this run (the earlier ~17k death was
  not reproduced — likely the window was closed), so it is live-inspectable for the comparison.

## ✅ ROOT CAUSE FOUND (2026-06-22 sess2) — missing splash-advance code at 0x80107xxx
Ran the oracle reverse-writer-hunt (wtrace_arm on `[0x1F800190,0x1F8001A8]` from a fresh boot;
TCP raw cmd since the client maps `wtrace_add` but the oracle wants `wtrace_arm`). The oracle's
state byte `[0x1F80019C]` ping-pongs EVERY frame:
- `PC 0x80107268` writes **state=2** (the advance trigger), then
- `PC 0x80050DA8` (state-2 handler) writes **state=1**.
So on HW, `0x80107268` runs every frame and drives the splash forward.

**The recomp NEVER runs it because the code isn't there.** Disassembled `0x80107200..0x8010728C`:
- ORACLE = the real splash-advance function (the `sb v0,412(v1)` state=2 write is at 0x80107268,
  preceded by a counter `addiu s1,s1,-1` / `bne s1` loop + GPU/struct setup).
- RECOMP = **ALL ZEROS (NOPs).** The function is simply not loaded.

Memory map sampling `0x80100000..0x80110000` (recomp vs oracle): recomp has `0x80104368` and
`0x8010A000` loaded (but with DIFFERENT content than the oracle) while `0x80107000`, `0x80110000`
are ZERO in the recomp and real code on the oracle. So the recomp's high-RAM overlay
(`0x80100000+`) is PARTIALLY loaded and DIVERGENT — a **load / relocation divergence during boot**:
the recomp failed to bring the `0x80107xxx` overlay region into RAM (or loaded it elsewhere).

This is the real softlock cause (NOT the JIT/cache — confirmed pure interp: overlay
`dispatch_native=0`, `dispatch_interp_fallback=125778`, sljit `live=0/compiles=0`, autocompile off).

### NEXT (loader hunt)
Find how the oracle gets `0x80107xxx` into RAM and why the recomp doesn't. It loads EARLY (oracle
has it by frame 630), so it's a boot/splash-setup load — a CD read OR a RAM→RAM memcpy/decompress.
The cd_read_log ring (256 entries) has already evicted the early loads, so capture from boot:
- Arm an always-on/early wtrace on `0x80107000..0x80108000` (writes that INSTALL the code) on BOTH
  the oracle and the recomp; compare. The oracle's installer write (DMA or memcpy store) PC + source
  tells us the load mechanism; the recomp either never does it (skipped load / wrong branch) or writes
  it to a different address (relocation/base divergence).
- Also dump the full early CD read log from boot (restart with cd logging) to see if a CD read to
  0x107xxx happens on the oracle but not the recomp.

## LOADER HUNT RESULT (2026-06-22 sess2) — it's a CD DMA load, late, that the recomp never gets
Armed `wtrace 0x80107000..0x80108000` on a fresh oracle from frame 411. Findings:
- The ONLY CPU-store writes caught to the region were the BIOS RAM-zeroing pass (PC `0xBFC0D864`,
  ra `0xBFC06BBC`, writing 0x00000000 to `0xA0107C00+` at frame ~848). NOT the code install.
- The real code at `0x80107268` is **still ZERO at oracle frame 2289**, but **present by frame 3960**
  — and the wtrace caught **ZERO CPU stores** carrying those code bytes. ⇒ the splash-advance code
  is installed by **CD DMA (channel 3, CD→RAM)**, not CPU stores. It loads LATE (≈frame 2289–3960),
  i.e. DURING the splash, not at initial boot.
- So **the CD is NOT fully ruled out** after all: the EARLY load (`0x41800+`, 250 sectors) works, but
  this LATER DMA load of the `0x80107xxx` overlay is the one the recomp never receives (its region
  stays zero even at frame 78k). cd_read_log cross-compare blocked by tooling (beetle has no
  `cd_read_log`; recomp dump came back empty once — Rule 15 follow-up).

### Refined model + next step
Likely chicken-and-egg / CD-completion: the splash state-0 loop (both run) initiates or waits on a CD
load that brings in the `0x80107xxx` advance task; on HW the DMA completes (~frame 3960) and the task
then drives state 2↔1; on the recomp the load never lands so state stays 0. This points back at the
**CD/DMA + completion-event path for a SECOND (mid-splash) load**, distinct from the working first load.
NEXT: (1) fix/confirm cd_read_log on the recomp + add it to beetle (Rule 15) so we can diff CD reads;
(2) restart the RECOMP with full from-boot CD logging and check whether it ever issues a read whose
DMA dest covers `0x107xxx` — if NO, a code-path/branch divergence skips the load (the state-0 loop
never reaches the load trigger); if YES-but-data-absent, a CD/DMA targeting or completion bug drops it.
(3) On the oracle, trace what the state-0 loop does right before the load fires (the CD command +
DeliverEvent/CdSync) and compare the recomp's same loop.

## RECOMP-SIDE RESULT (2026-06-22 sess2) — the recomp NEVER issues the 0x107xxx CD read
Restarted the recomp, dumped cd_read_log: total=2651 reads, dest range `0x41800..0x104368`,
distinct LBAs {16,18,24,373}. **`covers 0x107xxx = False`.** The recomp does the directory lookup
(LBA 16/18/373 → scratch buffer `0x104368`) but **never issues the actual file load to `0x107xxx`**,
then goes quiet (total stuck at 2651 — no further CD activity while softlocked). So this is a
**code-path / task divergence in the file-load DECISION**, not a DMA-targeting or CD-fidelity bug at
the read level (the earlier `0x41800` load proved raw CD reads land correct, advancing MIPS code).

So the chain is:
1. splash state-0 loop runs (recomp + oracle).
2. on HW something issues a CD load of the `0x80107xxx` overlay (the splash-advance task) mid-splash
   (~frame 2289–3960) and DMAs it in.
3. the recomp never issues that load → `0x80107xxx` stays zero → advance writer `0x80107268` never
   runs → state stuck 0 → softlock.

### Where the load-issuing task likely lives (next investigation)
The state-0 handler (`0x80050D44`) only renders + loops — it does NOT issue a CD load. So the load is
issued by ANOTHER task/callback during the splash that the recomp doesn't run. Strong candidates,
in order: (a) a per-frame scheduler/event/callback driven off the game's VBlank handler — SAME
neighborhood as the frame-1997 HookEntryInt handler, so the recomp's event/scheduler model may not
run the scene-loader task; (b) a CD-complete-event (CdSync/DeliverEvent) callback that chains the
next load; (c) a separate thread/TCB the recomp's scheduler never resumes.

### Concrete next steps
1. Trace the ORACLE's CD command stream (mmio writes @0x1F801801) around frame ~2289–3960 to find the
   SetLoc+ReadN that targets the 0x107xxx file, and the PC/caller that issues it. (recomp has
   mmio_dump / a CD command trace; beetle needs the same — Rule 15.)
2. Find that issuing PC/function; check whether the recomp ever executes it. If it's reached from a
   VBlank-callback / event chain, compare the recomp's event delivery + DeliverEvent/WaitEvent +
   TCB/callback execution rings to the oracle's at the splash.
3. If the issuing task is event/scheduler-driven, this likely shares root with the interrupt-model
   work — verify the game's per-frame callback chain fully runs in the recomp.
Tooling debt (Rule 15): raw-socket cd_read_log parse fails on the big response (use debug_client);
beetle lacks cd_read_log + a cdrom command trace — add them for clean cross-backend diffs.

## DMA-3 trace + refined divergence point (2026-06-22 sess2)
Armed `wtrace 0x1F8010B0` (DMA3/CDROM MADR) + `0x1F801800` (CD cmd) on a fresh oracle. During the
splash the oracle does HEAVY CD streaming: DMA3 MADR is written ~1.1M times (to `0x1A2xxx` /
`0x1A8xxx-0x1AExxx`, all via game DMA routine `pc=0x8008DB88`) — this is the splash's CD-DA / XA
AUDIO (the Whoopee-Camp jingle, TRACK 02). The single `0x107xxx` code-load DMA is buried among them
and evicted from the (small) ring before I can dump it — wtrace-on-MADR can't isolate it by value.

**Refined divergence point (the precise lead):** the recomp's cd_read_log shows it DID the directory
lookup (LBA 16/18/373 → scratch `0x104368`) **recently, right before it got stuck**, then CD activity
FROZE (total stuck at 2651). The oracle does the SAME lookup and then issues the `0x107xxx` file load
+ keeps streaming. So the divergence is **immediately after the directory lookup: the recomp does not
follow it with the file load.** Two sub-cases to decide:
- (a) the recomp's directory-entry RESULT (the LBA 373 / `0x104368` record: file extent-LBA + size)
  is WRONG → it computes "nothing to load" / wrong target. (Raw CD fidelity is OK — the 0x41800 load
  was valid code — but verify the specific LBA-373 record content vs the oracle.)
- (b) the record is correct but a later branch (a state/flag/event check after the lookup) diverges so
  the load is skipped.
Also notable: the recomp's cd_read_log dests are only `0x41800`/`0x104368` — NOT the `0x1Axxxxx` audio
stream the oracle hammers — so the recomp may also not be streaming the CD-DA/XA splash audio (could be
a related symptom of the same post-lookup divergence, or a separate XA path difference).

### Concrete next step
Restart the RECOMP with `wtrace` on the CD command reg `0x1F801801` (+ SetLoc params `0x1F801802`) and
on `0x1F8010B0` from boot; catch the SetLoc+ReadN sequence for LBA 16/18/373 and the issuing PC/ra.
That PC is the directory-lookup/file-open routine. Then disasm it (live RAM) and single-step/trace what
it does AFTER the lookup — find the branch where the oracle issues the next ReadN (file→0x107xxx) and
the recomp doesn't, and read the directory-entry record both sides consume. That pins (a) vs (b).

## SUB-CASE (b) CONFIRMED — directory read is VALID; it's a file-search/load-DECISION divergence
Decoded the recomp's `0x104368` buffer: it is a well-formed ISO9660 directory: rec0 = {len 48,
extent_LBA 373, size 2048, flags 0x02 (directory), name "."}, rec1 = {extent_LBA 22, ".."}, more
records follow. So the recomp READ AND PARSED the directory at LBA 373 correctly — the data is fine,
CD fidelity is fine. ⇒ The divergence is **(b): after a correct directory read, the code that walks
the directory to find the target file and issue its load (→0x107xxx) takes a different path in the
recomp and never issues the ReadN.** So the next investigation is the FILE-SEARCH / LOAD-DECISION
code, not the CD layer.

Likely shapes of the (b) divergence: a filename compare that mis-matches; a directory-walk loop that
exits early; a "load already done?" / state / event-flag check that's in the wrong state in the recomp;
or a return value from the dir-read/open routine that differs. Find the routine that issued the LBA-373
read (wtrace CD cmd reg from boot → PC/ra), disasm it live, and compare its post-read branch + the
values it tests on recomp vs oracle.

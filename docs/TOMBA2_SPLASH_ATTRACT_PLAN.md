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

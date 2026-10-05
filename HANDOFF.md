# SESSION HANDOFF — 2026-10-03

Session that closed #5 and #4 (both false positives), repaired two audit tools,
and root-caused #10. **Read this first, then `ISSUES.md` for detail.**

This is a *session* handoff. The durable records are `ISSUES.md` (per-issue
truth), `BFM_WIDESCREEN_PLAN.md`, and `FAITHFUL_TIMING_PLAN.md`. If this file
and `ISSUES.md` ever disagree, **`ISSUES.md` wins** — it is the one that was
corrected in place each time a conclusion was overturned.

---

## TL;DR

| | |
|---|---|
| Closed | **#5** (mid-func dispatch) and **#4** (unemitted GTE) — both were **false positives** |
| Root-caused | **#10** — the guest executes `jr $0`. Nothing is malfunctioning |
| Fixed | 3 broken tools that had been silently lying (see below) |
| Retracted | 3 of my own wrong claims, all corrected in `ISSUES.md` |
| Still open | #1, #3, `0x800CAE80`, and one `scratchpad` question that decides #10's real fix |

**Nothing user-visible got faster or better this session.** The gain is that the
board is finally trustworthy: three issues that looked like defects were not,
and the one real bug is now down to a single instruction with a known cause.

---

## The single most important thing to carry forward

**`ISSUES.md` was wrong about #5 and #4, and both were "found" by tools that
were broken.** Before trusting any audit finding in this repo, confirm the tool
actually produces non-zero output on the construct it claims to watch. A pass
that reads zero because the emitter stopped emitting that construct is
indistinguishable from a pass that found no bugs — and here it was mistaken for
a bug report three separate times.

Concretely: `codegen_audit_game.py` was matching **0 of 15606** real dispatch
cases and inventing **929 phantom misses**. Its passes `[2]`/`[3]` are still
**vacuous** on BFM (no `call_by_address` is emitted at all) and now say so
out loud. Its `CLEAN`/exit-0 means **"0 known defects"**, never "no defects".

---

## #10 — the one real bug, and why the obvious fix is wrong

Reproduced twice, control-first, same process and binary:

```
savestate: LOADED slot 1 -> resuming pc=0x80042558   (static text) -> keeps running
savestate: LOADED slot 4 -> resuming pc=0x8017F8E8   (overlay)     -> FATAL PC=0
```

**Cause, proven offline from slot04's own bytes:** the resumed function's
epilogue reloads `$ra` from the stack and that slot is zero.

```
sp = 0x1F800240
0x8017F910: lw    $31, 28($sp)   ->  scratchpad[0x25C] = 0x00000000
0x8017F91C: jr    $31            ->  jump to 0x00000000
```

`pczero_addr=0x8017F900` is exactly the block containing that sequence.
**Dispatch, overlay loader, interpreter and call-contract all executed the guest
faithfully.** The plan the user approved ("A then B") is **wrong and must not be
built** — there is no routing bug to fix:

- **(A) snapshot entry + intra-function offset** — impossible: `overlay_find_by_range`
  returns `-1` and `sljit_try` returns `compiled:0`, so there is **no registered
  entry to take an offset against**.
- **(B) route mid-function overlay addrs to the interpreter** — already happens,
  and it works; the interpreter reaches the `jr` and only dies there.

### The one measurement that decides the real fix

Not answerable from this repo alone. Three possibilities, **do not guess**:

1. The state is genuinely degenerate → runtime is faithful, and the actual bug is
   that **savestate is offered at unsafe freeze points**.
2. **SPAD save/restore is defective** → the value was live and the snapshot lost it.
3. Captured mid-frame at a point normal play never reaches → resuming is meaningless.

**(2) first — it is ~15 minutes and rules out a real runtime bug:**
write a known pattern to scratchpad → `savestate save` → `savestate load` →
read back. If it round-trips, the capture is innocent.
Then **(1) needs the Beetle oracle** on port 4380 (CLAUDE.md §16) — it is the
only thing that can say what real hardware held at that instant.

---

## Repro recipe (the part that was painful to find)

The three states are in `probes/` (gitignored; **copy them to the repo root**,
which is `memcard_dir`, before running). All three share identical integrity
fields, so they came from one build.

```bash
cp probes/state_80010000_slot0{1,4}.pst .
PSX_EXIT_HALT=1 setsid ./runtime/build/Brave_Fencer_Musashi__PSXRecomp_ \
  --headless --no-launcher --game games/musashi/game.toml \
  --disc "games/musashi/Brave Fencer Musashi (USA).cue" --debug-port 4370 \
  > /tmp/r.log 2>&1 < /dev/null &          # ~35 s to reach a loadable state

python3 tools/debug_client.py savestate load 1   # control: runs
python3 tools/debug_client.py savestate load 4   # overlay: FATAL PC=0
```

`PSX_EXIT_HALT=1` is essential — it halts and keeps serving over TCP instead of
shutting down, which is the difference between a corpse and a witness.

### Traps that cost real time — do not repeat them

- **One experiment per process.** The first failing load trips
  `psx_fatal_halt`, the guest freezes, and every later `savestate load` is a
  **silent no-op**. This produced a fake `hits:0` that nearly became a theory.
- **Arm instrumentation BEFORE the load**, in the same live session.
- `dispatch_check` answers *"was this recently dispatched"* against a bounded
  ring, and returns `found:false` for `0x80042558` — a PC that demonstrably
  works. **Never read its zeros as "undispatchable".**
- `debug_client.py` sends hex as a **quoted** string; handlers using hand-rolled
  `strstr`/`strtoul` only accept unquoted numbers. Use a key=value arg the
  client converts to a string, and if a probe says "need addr", suspect this.
- **A negative result from an unverified instrument is not evidence.**

---

## Tools fixed (all were silently lying)

| tool | was | now |
|---|---|---|
| `tools/codegen_audit_game.py` | matched 0/15606 dispatch cases; 929 phantom misses | correct; vacuous passes self-report |
| `tools/debug_client.py` | no `savestate` binding — the whole save/load flow was undrivable | bound |
| `handle_sljit_try` | `strtoul` rejected quoted hex → `"need addr"` for every valid request | parses both shapes |
| `tools/gte_coverage.py` | fine, but its detection *method* was invalid (see #4) | unchanged |
| `tools/decode_pst.py` | **did not exist** | new; decodes a `.pst` from its bytes, self-checking layout |

No config in the tree had an `[audit]` block, so `codegen_audit*.py` refused to
run on *any* game. Added one to `games/musashi/game.toml` with the code region
read from the EXE header (`t_addr`/`t_size`), not assumed.

---

## Why #4 and #5 closed (both false positives)

- **#5** — the mid-function convergence pre-pass (`MAX_PASSES` 3→256) already
  prevents the unregistered `call_by_address` path from ever being reached. BFM
  has **0** in-code misses. The `register_cross_function_target` calls the issue
  asked for were never needed.
- **#4** — `tools/gte_coverage.py` reports **479/479** GTE instructions present,
  **0 missing**. All four MVMVA sites emit `gte_execute(cpu, 0x0480012)`.
  The issue's proposed test — *grep the generated C for the PC* — **cannot
  work**: generated C is a translation that annotates PCs only where it chooses.
  **#3 (missing PS logo) is NOT closed by this**; only its proposed cause died.

---

## Open, ranked

1. **#10** — the scratchpad round-trip test, then the oracle. See above.
2. **#1** — per-card directory load never runs; Phase 4 blocker, root cause
   narrowed, has a "Concrete next step" at `ISSUES.md:216`.
3. **`0x800CAE80`** — a `jal` target sitting in the **358 KB gap** between text
   end (`0x80074800`) and overlay[1] base (`0x800CE000`), covered by nothing.
   I chased a digit-transposition theory here and **it was wrong**: the emitter
   correctly implements `(PC+4)&0xF0000000 | idx<<2`. Still unexplained — trace
   the `func_8001FC08` call site at runtime before treating it as live.
4. **#3** — missing PS logo glyph. Cause removed, bug untouched.
5. **Reveal-margin black patches** (`BFM_WIDESCREEN_PLAN.md`) — the only
   player-visible defect. Its discriminating test is written down and **never
   run**: read each margin prim's colour word at its census `src_addr` vs centre
   prims. Colour ~0 ⇒ depth cue (fix the fade distance); normal ⇒ hypothesis
   dead. **Measure before building** — several confident readings this project
   survived only until a screenshot killed them.

---

## Working notes

- Headless works and there is a real display (`:0`); no window needed:
  `--headless` implies `--no-launcher`.
- Debug server on 4370. `tools/debug_client.py <cmd>`; the client has a raw
  fallback, so **every** server command is reachable as `cmd key=value` even
  with no binding.
- `dispatch_census`-style game audits need `[audit] regions` in the game's toml.
- Game text region for BFM: `0x80010000–0x80074800` (from the EXE header).
- Build: `ninja -C runtime/build` — green as of this session.

## If you only remember five things

1. #5 and #4 were **false positives**; the tools that "found" them were broken.
2. `codegen_audit_game.py` `CLEAN` means **0 known defects**, not "no defects".
3. #10's cause is `jr $0` from a **zero saved-`$ra` stack slot** — do **not**
   build the entry+offset fix; there is no entry to anchor to.
4. **One experiment per process**, armed before the load, or you will "measure" a
   frozen guest and believe it.
5. When in doubt, **produce the artifact** (CLAUDE.md Rule 14). Three of my wrong
   claims this session all died the moment something was actually measured.

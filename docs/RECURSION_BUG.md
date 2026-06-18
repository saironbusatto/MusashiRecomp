# Tomba long-run recursion freeze — working doc

Branch: `bug/recursion`. This is the single source of truth for the long-run
freeze investigation and the soak-harness regression that is currently blocking
reproduction. Update it as we learn.

---

## 1. The bug we are actually chasing (the freeze)

**Symptom.** Tomba runs correctly for a long time, then hard-freezes. Intermittent;
*often sooner than 15 minutes* but **not reliable**. The captured specimen is
`_freeze_specimens/tomba_4393_freeze_46440.dmp` (frame 46440).

**Root cause (from the minidump).** A **runaway guest recursion that never unwinds.**
The per-frame game-flow state machine re-enters *itself* across the
**compiled ↔ dirty-RAM-interpreter boundary**. Validated, symbolized cycle:

```
exec_one (main loop 0x8001A51C)
  -> psx_dispatch_game_compiled(0x8001A954)
    -> 0x8001A954 -> 0x8001AC00 -> 0x8001B2B4 -> 0x8001B5A8 -> 0x80046264
      -> dirty_ram_dispatch
        -> psx_dispatch_game_compiled(0x8001A954)   <-- back to the top
```

Repeats ~1443 times -> native **fiber stack overflows** -> stack-depth guard halts
the process (`psx_fatal_halt`, which parks or exits). The state-machine fields
`[+0x4a]/[+0x4c]/[+0x4e]` on the controller object at `0x801FD800` are pinned at
`1/1/1` and never advance — that is the cycle that won't terminate. The ~1443
frames overflow nearly instantly *once the cycle starts*; the "~15m / intermittent"
is how long until the **trigger condition** is met, not how long the recursion runs.

**What it is NOT** (all prior hypotheses, refuted):
- NOT data corruption — `0x8001a954` is not a wild pointer; it exists nowhere in
  RAM as data.
- NOT nested interrupt delivery — `in_exception=0`, exception counts normal.
- NOT a fiber deadlock/livelock.
- Scene id intact, table `DAT_8007c640` intact.

**Why it's slippery to capture.** `g_psx_dispatch_depth` reads only 5 — it is blind
to this, because the 1443 frames are direct `func_X(cpu)` calls it does not count.
The `recent_fn` ring is time-ordered, so it shows only the leaf churning at the trip,
**not** the recursing cycle. **This is the entire reason the native_stack tool
exists** (see §3).

**Suspected trigger.** CD-DMA dirty-page over-marking: `dma.c:695` marks every
DMA'd word executable/dirty and never clears it (463/512 RAM pages dirty at the
freeze), time-correlated with the onset of boundary re-entry.

**Fix territory (NOT yet done).** The interp<->compiled call/return contract
(`dirty_ram_dispatch` / `psx_dispatch_game_compiled`) must **unwind** at the
boundary instead of **nesting**; and/or stop `dma.c:695` marking non-code CD-DMA
data as executable/dirty. Same family as the "wild call contract" recursion bugs
(Bug A/C/D). The fix is in the **recompiler**, never in generated C.

---

## 1a. Confirmed organic capture — frame 50241 (`build-recursion`, 2026-06-17)

Second specimen, captured live by the 4-soak harness + harvest watcher
(`_freeze_specimens/soak_report_20260617_171145.json`). The native_stack tool
worked end-to-end (walked 300000 frames, decoded the cycle); the early-flush was
not even needed. Confirms §1 and adds the guest-level mechanism:

- `reason`: native-stack guard tripped — runaway guest recursion.
- `recursion_func = 0x8004DEE0`. At halt: **`v0 = 0x8004DEE0`, `ra = 0x8004DEE8`**
  (`= v0 + 8`) -> the guest ran a `jalr v0` whose target slot points back into the
  recursing chain. **Indirect call through a jump-table / function-pointer slot**
  — the wild-call family (cf. seesaw `func_8001DFD4`, bogus `v0=0x8001DFF8`), NOT a
  benign loop. `pc = 0x00000000` = the dispatch `pc==0` unwind sentinel.
- Steady-state lap (~23074x, ~13 host frames each):
  `exec_one -> dirty_ram_dispatch -> psx_check_interrupts -> psx_dispatch_impl ->
   func_80046264 -> func_8001B5A8 -> func_8001B2B4 -> func_8001AC00 -> func_8001A954
   -> exec_delay_slot -> psx_dispatch_game_compiled -> exec_one`.
- Dirty pages on the interpreted path: **`0x80063xxx` and `0x8011xxxx` (overlay
  region)**; `last_store_pc = 0x80116A24`; `dirty_block_tail` tail shows
  self-referential dispatch (`target == ra == 0x80116204`).

**Mechanism, restated with evidence:** a guest indirect transfer (jalr through a
jumptable/fp slot) crosses the compiled<->dirty-interp boundary, and the recompiler
implements each crossing as a **nested host call** (`psx_dispatch_game_compiled` /
`dirty_ram_dispatch` / `exec_one`) that never returns. The guest transfer is
jump/tail-style (guest stack does not grow proportionally) but the HOST stack grows
~13 frames/lap -> fiber overflow. CD-DMA dirty-marking (`dma.c:695`) is what forces
those overlay/0x80063xxx pages onto the interpreted path in the first place.

---

## 2. The reproduction harness ("soaks") — and the regression blocking it

**Workflow.** Run **4 instances** in parallel ("Soak A/B/C/D"). Claude launches
them; the user navigates **2 to a New Game** and **2 into Dwarf Forest** (overworld),
then lets all four **idle**. Running 4x in parallel is how we hit the intermittent
freeze fast. Per-instance memcard dirs (`saves_a..d`), **software renderer**, 4:3,
ports 4393-4396 (`tomba_soak_{a,b,c,d}.toml`). Stay off heavy host work while
soaking (a wall-clock starvation watchdog, 4s no-heartbeat -> exit(2), false-trips
under host load).

**The regression — RESOLVED.** It was a **stale build artifact** (`build-soak`),
NOT the tooling and NOT master. `build-soak`'s generated `.obj` were byte-identical
to the good build, but its *runtime* objects predated the session and the link was
incoherent; a clean from-scratch rebuild (`build-recursion` = master + native_stack
tooling + early-flush) does **not** crash on overworld-load (user-confirmed). The
"crash the instant anything loads" symptom is gone. See §4 / §5.

---

## 3. The native_stack tool (the thing that may be causing the regression)

Uncommitted tooling added to `runtime/src/crash_trace.c` (129 lines): a
host-stack walker `append_native_stack()` that recovers the true recursion cycle
(the `recent_fn` ring can't — see §1). It is called **only** from
`psx_crash_trace_dump()`, i.e. only while a crash/fatal/SEH dump is already in
progress. It walks from the faulting/the current `rsp` up to the fiber `StackBase`,
keeps only validated return addresses (value in `.text` preceded by a `call`),
run-length-collapses them, and emits module-relative RVAs + a histogram. Every
stack read is bounded by `VirtualQuery`; symbolize offline with `nm`
(`_freeze_specimens/analyze_named.py`, `decode_report.py`).

**Known hazard (handoff issue #3).** Both `psx_signal_handler` and
`psx_seh_handler` run **on the faulting stack** and call the dump. If the original
fault is the recursion's stack overflow, `append_native_stack` runs on the
already-exhausted stack and can **re-fault** — taking the whole report with it
(silent SIGSEGV, no JSON). As written, the tool meant to capture the overflow can
**destroy the very report it was built for.**

---

## 4. Evidence collected this session (artifact-level)

Compared `TombaRecomp/build-soak` (A, has tooling, crashes) vs
`TombaRecomp/build-soak-e` (B, pure master, healthy). MD5'd all 49 objects in each:
**only two differ** — `crash_trace.c.obj` and `main.cpp.obj`. `main.cpp` is an
AppImage `getenv("APPIMAGE")` change, **inert on Windows**. All generated BIOS/game
code (`SCUS_942.36_*.c.obj`, `SCPH1001_*.c.obj`) is **byte-identical MD5**. The
psxrecomp reflog shows HEAD parked at master `3ba40b0` since 09:34 that day, so A
and B were regen'd off the **same** master tip — **not** a flavor trap, **not**
stale generated code. (Those were the handoff's two hypotheses; both refuted.)

**Open logical tension — RESOLVED.** The tension (`crash_trace.c`'s only new code
runs in the terminating dump path, yet B "got into game") dissolved when a clean
rebuild WITH the tooling did not crash. The crashing `build-soak` was a stale/
incoherent incremental build; neither the tooling nor a master regression was at
fault. The session pivoted from "explain the overworld-load crash" to "capture and
fix the actual freeze" — which we then did (§1a, §7, §8).

---

## 5. Status & plan (updated 2026-06-17, end of session)

**Done this session:**
- Branch `bug/recursion` created; native_stack tooling committed (`d9b6ae5`).
- native_stack tool **hardened** with an early-flush (commit `e63fdee`): the report
  is written to disk BEFORE the risky host-stack walk, so a walk fault can no longer
  destroy it. (Turned out not to be needed for the captures — the walk completed —
  but it's the right robustness fix and unblocks future overflow captures.)
- Overworld-load regression (§2) **resolved** — stale build artifact; clean
  `build-recursion` is healthy.
- Freeze **captured organically** (3 specimens: A frame 50241, C frame 50088, B
  Dwarf Forest) via the 4-soak harness + `harvest_soak.py`. Deterministic: all three
  halt with identical `v0=0x8004DEE0, ra=0x8004DEE8, pc=0`. Both New-Game and
  Dwarf-Forest areas reproduce → universal per-frame code path.
- Root-caused to the interpreter↔compiled boundary (§7) + Ghidra-mapped the guest
  state machine (§8). External review (§9) rates this Hypothesis A; flags a distinct
  Hypothesis C to rule out with an oracle trace (§10).

**Builds in tree (do NOT delete):** `build-recursion` (X = master+tooling+early-flush,
the good build), `build-soak`/`build-soak-e` (the A/B subjects). Soak configs
`tomba_soak_{a,b,c,d}.toml` (ports 4393-6, software, per-instance `saves_a..d`
seeded from the real card). `harvest_soak.py` watches the shared
`psx_last_run_report.json` and decodes with `allsyms.txt`.

**Next (in order):**
1. **A-vs-C static check:** disassemble the dynamic routine around `0x80063864`
   (the dirty page `func_80046264` first calls into), specifically the instructions
   before/after its transfer to `0x8001A954` and its stack-restoration / intended
   return path (§13). Is the re-entry a legit guest tail-transfer (A) or an interp
   artifact (C)? `0x80063xxx` is runtime-loaded — read from a PARKED instance via the
   debug server `read`, not the static EXE.
2. **Oracle disambiguation (§10):** trace control flow at `0x8001A954` / `0x80046264`
   / `0x80063864` / the interpreted `jal 0x8001A954` vs psxref @ 4380. Strongest
   metric: `main_loop_entries_per_vblank`. Cheap pre-check: on the oracle, sample
   `state[0x4a]` + `0x8009BCDD` over minutes of play — if they sit unchanged for
   thousands of frames, Hypothesis B is falsified outright.
3. **Implement the single mixed-dispatch-owner fix (§11)** — RUNTIME-only
   (`dirty_ram_interp.c` + `cpu_state.h`), so **rebuild, no regen**. Re-soak all 4
   (New-Game area ≈ 14-min repro) to validate the freeze is gone.
4. **Separately:** tighten CD-DMA dirty-marking (§12) — not the root cause but it
   makes the broken boundary common.

No stubs, no HLE, never edit generated C; the fix is in the recompiler/runtime.

---

## 6. Build & run (this project)

```
cd /f/Projects/psxrecomp/TombaRecomp
export PATH=/c/msys64/mingw64/bin:$PATH TMP=/c/msys64/tmp TEMP=/c/msys64/tmp
# regen tools + BIOS + game (generated/ is gitignored; off master tip):
cmake --build ../psxrecomp/recompiler/build --target psxrecomp-game psxrecomp-bios -j8
(cd ../psxrecomp && ./recompiler/build/psxrecomp-bios.exe --config bios/SCPH1001.toml)
../psxrecomp/recompiler/build/psxrecomp-game.exe --config tomba_soak_e.toml
# build:
cmake -S . -B <build-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=ON -DPSX_LAUNCHER=OFF \
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe
cmake --build <build-dir> --target psx-runtime -j8
# run (per-instance card; taskkill first):
taskkill //F //IM psx-runtime.exe 2>/dev/null
nohup ./<build-dir>/psx-runtime.exe --game tomba_soak_a.toml > _soak_a.log 2>&1 &
# decode a freeze report's native_stack:
nm -n <build-dir>/psx-runtime.exe | awk '$2~/^[Tt]$/{print $1,$3}' > ../_freeze_specimens/allsyms.txt
python ../_freeze_specimens/decode_report.py psx_last_run_report.json ../_freeze_specimens/allsyms.txt
```

**Hard rules:** fix the recompiler, never generated C; no printf/log files (TCP
debug server + always-on rings; read `psx_last_run_report.json` for fatals);
per-instance memcard dir per concurrent instance (sharing corrupts the card ->
fake "regression"); software renderer for soaks; `taskkill //F //IM psx-runtime.exe`
before any launch.

---

## 7. The runtime contract — why the host stack leaks (the mechanism)

The codebase ALREADY solves tail-calls for **compiled** code, via two pieces:

- **`psx_dispatch_impl`** (generated; emitted in `recompiler/src/full_function_emitter.cpp:1103`)
  is a **tail-call trampoline**:
  ```c
  for (;;) {
      cpu->pc = 0;
      dispatch(addr);                 // run one compiled func, or interp a dirty block
      if (g_psx_call_bail) { ...resolve/propagate/flatten... }
      if (cpu->pc == 0) { ...contract check; return... }   // genuine return
      addr = cpu->pc;                 // TAIL call: re-dispatch in-place, NO host nesting
  }
  ```
  A compiled function signals a tail-call by setting `cpu->pc` and returning; the
  loop re-dispatches without growing the host stack. Compiled tail-call loops stay flat.

- **`psx_call_contract`** (`runtime/include/cpu_state.h:132`, static inline) is the
  wild-return unwind. After a direct call returns, if guest `$sp`/`$ra` don't match
  the call site, it raises `g_psx_call_bail = 1`, sets `cpu->pc = $ra` (the true
  target), and unwinds frame-by-frame until the frame whose `(ra,sp)` matches
  resolves it (clears bail, resumes).

**The dirty-RAM interpreter BYPASSES both.** In `runtime/src/dirty_ram_interp.c`,
`exec_one`:
- guest `JR`/`J` (case 0x08 ~L550, 0x02 ~L697): correctly unwind — `cpu->pc=target; return 1`.
- guest `JAL`/`JALR` (case 0x03 ~L703, 0x09 ~L556): **NEST** — call
  `psx_dispatch_game_compiled(cpu, target)` directly. The post-call
  `psx_call_contract(...)` is only reached if that nested call RETURNS.
- `dirty_ram_dispatch_inner` (~L1170-1186) does the same for a surfaced compiled
  target: nests `psx_dispatch_game_compiled` instead of returning `cpu->pc=target`
  to the trampoline.

So when interpreted overlay code does `jal func_8001A954` (tail-transfer back into
the per-frame loop), the interpreter nests a host frame. The previous lap's compiled
chain (`func_8001A954 → … → func_80046264`) is still suspended (it called *into* the
interpreter and is waiting for a return that never comes). Every lap leaks the chain;
the `psx_call_contract` that would unwind it is **unreachable** because the nested
`psx_dispatch_game_compiled` never returns (it descends forever).

**Proof it's host-only, not real guest recursion:** at the halt the guest
`sp = 0x801FE338` — only ~7 KB below the stack base `0x801FFFF0` — and it does **not
grow** across all ~23,074 laps. The guest is *iterating* (tail dispatch); the host
is *recursing* (~13 frames/lap).

---

## 8. The guest state machine (Ghidra: `SCUS_942.36_no_header`, base `0x80010000`)

Two nested selectors, both pinned at the freeze:

**Selector 1 — `func_8001A954` (per-frame state dispatcher).** Reads
`state[0x4a]` (halfword) from the object pointer `*(0x1F8001D4)` and switches:
`0 → func_8001A9F0`, `1 → func_8001AC00`, `2 → func_8001D2F0`, `3 → func_8001D480`,
else return. Stuck at **`state[0x4a] == 1`** → calls `func_8001AC00` every frame.
It only *reads* the selector; it never writes it. (Adjacent fields
`[+0x4a]/[+0x4c]/[+0x4e]` read `1/1/1` and never advance.)

**Chain:** `func_8001AC00 → func_8001B2B4 → func_8001B5A8 → func_80046264`.

**Selector 2 — `func_80046264`.** Runs a per-frame call sequence (incl. the dirty
hop `jal 0x80063864` first), then jump-table dispatches on a **byte at `0x8009BCDD`**
(16 cases, table at `0x80013C60`, computed `jr v0`). Stuck case →
`0x8004DEE0` stub (`jal 0x8004DFA0; move a0,s0; j 0x8004DF68`). Also at `0x80046340`
it reads a word at `0x8009BCC8` (scene id, reads 0) and one branch calls overlay
`0x8011AF78`. A third byte selector sits at `0x800A539A` inside the `0x8004DEx` stubs.

**`func_8004DFA0` (the stuck case's handler):** a per-object GPU/GTE render routine
— GTE transforms, appends primitives to the ordering table at `*0x1F800164`, loops
over vertices, `jr ra`. Returns normally; advances **no** selector.

So nothing in the hot loop (`func_8001A954`, `func_80046264`, `func_8004DFA0`)
advances either selector. Whatever flips them (an event/input/animation/CD-script
step) lives elsewhere.

---

## 9. External review verdict (fresh-context second opinion)

- **Hypothesis A (interp↔compiled boundary nesting) — overwhelmingly likely.**
  Mechanically exact: flat guest `sp` + host `+~13/lap` + the unreachable contract
  check fully explain "thousands of successful iterations, then overflow."
- **Hypothesis B (HW/event state wedge) — no positive evidence.** `state[0x4a]==1`
  plausibly just means "active gameplay" and can stay 1 the entire session.
  `0x8009BCDD` indexes a **render handler** (`func_8004DFA0`) → looks like an
  object/render-TYPE selector, not a temporal counter; an object using the same
  renderer every frame for its whole lifetime is normal. Adjacent `1/1/1` may be
  flags, not counters. Normal gameplay continuing right up to the guard trip favors
  A (a real wedge would show visible guest-level nonprogress *before* the host stack
  happened to exhaust).
- **Hypothesis C (NEW, must be ruled out) — interp control-transfer bug.** The
  interpreter could mishandle a JAL delay slot / return-PC / a subsequent JR and
  **fabricate** the re-entry of `0x8001A954`. The flat guest stack does NOT rule
  this out. A short oracle PC-trace distinguishes C from A.

Conclusion: fix/neutralize the boundary stack leak (A) before any broad IRQ/event
hunt (B); run the oracle trace to exclude C first.

---

## 10. Oracle disambiguation plan (psxref @ 4380)

**Cheap pre-check (falsifies B without reproducing the freeze):** on the oracle,
sample `state[0x4a]` (via `*(0x1F8001D4)+0x4A`) and the byte at `0x8009BCDD` over
several minutes of ordinary gameplay. If they commonly sit unchanged for thousands
of frames, the "stuck selector" premise is dead.

**Decisive control-flow trace.** At each entry to `0x8001A954`, `0x80046264`,
`0x80063864`, and the interpreted `jal 0x8001A954`, record: `emulated_cycle`,
`vblank_serial`, `pc`, `ra`, `sp`, `*(0x1F8001D4)`, `state[0x4a]`,
`*(uint8*)0x8009BCDD`, `I_STAT`, `I_MASK`. Plus control-transfers only:
`source_pc`, opcode kind (J/JAL/JR/JALR), `target`, `new_ra`, `sp`. Compare a few
hundred laps recomp vs oracle:

| Result | Verdict |
|---|---|
| Same PC/target/RA/SP sequence (incl. repeated `0x8001A954` entries) | **A** — guest valid, only host representation leaks |
| First divergence at JAL/JR target, RA, delay-slot result, or return PC | **C** — interp control-transfer semantics wrong |
| Flow agrees, then a memory/IRQ value differs and only recomp stays in the loop | **B** — HW/event state divergence |

Strongest single metric: **`main_loop_entries_per_vblank`** (same count + flat guest
sp on both ⇒ selectors are irrelevant to the stack bug). Trace the **pointer** at
`0x1F8001D4` too, not just `[ptr+0x4A]` — the object may be replaced and each
replacement may start in state 1.

---

## 11. Fix design (single mixed-dispatch OWNER) — RUNTIME-only, no regen

Core rule: **exactly one owner of mixed-mode dispatch per native call chain.**
`exec_one` must never blindly call `psx_dispatch_game_compiled` in a way that can
establish another interp→compiled→interp chain.

Make the bail **typed** and **per-`CPUState`** (not a global bool):
```c
typedef enum { PSX_BAIL_NONE=0, PSX_BAIL_WILD_RETURN, PSX_BAIL_MIXED_TRANSFER,
               PSX_BAIL_EXCEPTION, PSX_BAIL_STOP } PsxBailKind;
// in CPUState: PsxBailKind bail_kind;  bool mixed_dispatch_active;
```
The first `dirty_ram_dispatch` becomes the owner (`mixed_dispatch_active=true`). A
nested boundary crossing does NOT start a new loop — it publishes the target and
unwinds to the owner:
```c
if (cpu->mixed_dispatch_active) { cpu->pc = target;
    cpu->bail_kind = PSX_BAIL_MIXED_TRANSFER; return 1; }
```
The owner re-dispatches in a `for(;;)` loop (compiled or interp), consuming
`PSX_BAIL_MIXED_TRANSFER` to continue and other bail kinds to unwind. Real returns
to the original compiled caller still return normally; native depth stays bounded.

Supporting changes:
- **Externalize interpreter continuation** into an `InterpState` (pc, pending-load
  reg/value/valid, cycles); yield only AFTER fully committing the control-transfer
  instruction + its delay slot.
- **Replace the overloaded `cpu->pc==0` sentinel** with an explicit
  `DispatchResult{kind,target}` where practical — far easier to reason about than
  `pc` + return value + a global bool.

**Regression-prone cases to test** (this is the Bug A/C/D contract area):
JAL/JALR delay-slot exec before yield; JR/JALR target captured before delay-slot
mutates the source reg; JALR with `rd`==`rs` aliasing; MIPS load-delay commitment;
exceptions in a branch delay slot (EPC/BD bit); cycle/event accounting exactly once
across a yield; interrupts arriving while a mixed-dispatch owner is active; guest
returns through KSEG aliases; **wild returns must pass THROUGH the owner, not be
consumed by it**; generated code caching guest regs in native locals across calls;
the `pc==0` overload.

**Long-term (bigger codegen change, not now):** continuation-passing / resumable
compiled frames — a compiled block returns the next guest PC/result and a central
dispatcher chooses compiled vs interp — eliminates host guest-call mirroring
entirely. The single-owner boundary is the lower-risk bridge for the current design.

---

## 12. CD-DMA "dirty/executable" marking (`dma.c:695`) — contributor, not root cause

The CD-ROM DMA loop marks **every** word it writes executable/dirty
(`dirty_ram_mark_executable_range(addr, 4)`). It conflates three properties:
*written* vs *modified-relative-to-compiled-bytes* vs *executed*. CD DMA carries
textures/audio/compressed data, not just code. This is NOT the stack-leak root
cause, but it makes the broken boundary common (page-wide dirtiness forces static
code onto the interpreter when unrelated data shares a page).

Better model (aligns with the overlay-cache work): per-region **write
generation/version** + compiled-code **byte-range + expected hash** + **ever-executed**
flag + dynamic translation cache. On ANY write (CPU/CD-DMA/other DMA/decompressor/
BIOS copy), invalidate only the OVERLAPPING compiled ranges. On dispatch: if a
compiled version exists and bytes/generation match → run compiled; else
interpret/compile-cache this byte-version. "Executable" becomes true on instruction
**fetch**, not on write. Natural end state: `address + content-hash → compiled
specialization`, with CD DMA merely one way a new byte-version arrives.

---

## 13. Immediate next disassembly target

The complete **dynamic routine around `0x80063864`** — the dirty page that
`func_80046264` first calls into — especially the instructions **before and after
its transfer to `0x8001A954`**, plus its stack restoration and intended return path.
This is the static-side A-vs-C discriminator: is the re-entry of `0x8001A954` a
legitimate guest tail-transfer (A), or does the interpreter fabricate it via a
mishandled delay-slot / RA / JR (C)?

`0x80063xxx` is runtime-loaded/dirty, so it may NOT match the static EXE — read it
from a **parked instance** (`python tools/debug_client.py --port 4393 read 0x80063864 <len>`,
length in DECIMAL) after re-soaking to a freeze, or from a captured overlay dump.
Earlier parked read of `0x800638C4` showed a clean GTE-loader leaf
(`lw t0..t2; mtc2 ×3; jr ra`), so at least part of `0x80063xxx` is well-formed code.

---

## 15. Strategy reset — control-flow flight recorder (session 2 end; ChatGPT-assisted)

The oracle RAM-diff of two **separately-navigated** idle emulators was a dead end
(alignment noise; §14). New plan (external review): **stop diffing RAM; build a
triggered control-flow flight recorder on OUR side, find the edge that flips at
~frame 50k, then walk a backward causal slice.** Use the oracle only at the END,
for a small causal address set (not whole-RAM).

**My analytical narrowing (reason first, then confirm with the recorder):**
- The 23k laps are **WITHIN ONE FRAME**, not accumulated across frames: the host
  call stack resets to the SDL frame loop every frame (the per-frame dispatch
  returns, then present, then next frame), so host depth CANNOT accumulate across
  frames. So a single frame's per-frame-update spirals to 23k and never returns.
- The trip cycle advances `psx_advance_cycles(1)` + `psx_check_interrupts` **per
  lap**, so 23k laps ≈ **23k guest cycles ≪ ~565k cycles/frame** → **no VBlank
  can fire before overflow** (~40µs guest time). So it is NOT "stuck waiting for a
  late interrupt" (scheduler-starvation-wait); it is a **control-flow cycle** — the
  per-frame update re-enters itself. => we are in ChatGPT's "branch/target changes
  abruptly → state/interp control-flow divergence" or "interpreter semantic bug" row.

**Decision tree (which problem do we have):**
| Observation | Diagnosis |
|---|---|
| depth grows gradually across frames; CF + scheduler stable | pure mixed-boundary stack leak |
| 23k laps in ONE frame; cycles/events don't advance | mixed dispatch starves scheduler |
| scheduler advances but a branch/target changes abruptly | state / interp CF divergence |
| branch operands identical but recomp picks a different target | interpreter semantic bug |
| recomp value differs, last-writer = a HW-produced write | hardware emulation bug |
| watermark unwind stops the outer pump (normal exec didn't) | bailout impl incomplete |

**Execution order (focused, NOT another broad diff):**
1. **Per-frame frequency + first-occurrence of the re-entry edge** (interp →
   `0x8001A954`) and the jumptable edge (`0x8004630C` → its `jr` target). Is the
   re-entry **ordinary bounded per-frame behavior that stops TERMINATING at ~50k**,
   or a brand-new edge? (ChatGPT expects at least one is normal rendering behavior.)
2. **Last-writer map** on the values controlling those edges (selector `0x8009BCDD`,
   the jump-table entry, `state[0x4a]`, the loop's termination variable): record
   `{pc, frame, cycle, old, new}` per RAM write; when a load feeds the controlling
   branch, emit a backward slice "edge taken because reg==V loaded from Y last
   written at PC Z." That answers *what changed and why*, vs 900 differing bytes.
3. **Indirect-site state** per branch: `{last_target, first_seen_frame,
   last_change_frame, counts_by_target}` — makes "did `func_80046264` start taking a
   new target at ~50k?" instantly answerable.
4. **Oracle, narrowly:** only after #1–3 name a small address set, ask Beetle
   (write-trace `trace_arm`/`ram_diff`, or a tiny **5-address PC hook** added
   locally inside the Beetle core — much smaller than lockstep) whether real HW
   takes the same edge, how often, and whether it ever transitions the controlling
   value the way we do. Anchor epochs on a once-per-update write (OT-cursor reset /
   game frame counter), local-align (allow ins/del), filter framebuffer/OT/stack/
   audio/RNG, compress repeated identical writes.

**Hooks available (runtime, no regen):** `s_frame_count` (main.cpp:1189),
`psx_cycle_count` (psx_cycles.c), the `interp_enter_compiled` boundary
(dirty_ram_interp.c — the re-entry edge `target==0x8001A954` passes through here),
`g_dirty_ram_flow_log` ring, `crash_trace.c` report dump. First instrument = a
re-entry-edge per-frame count ring dumped on the guard trip.

---

## 16. Flight-recorder runs #1/#2 — the recursion is VARIABLE; use the data-driven rings

Two turbo'd soaks with the per-frame site recorders (commits `2475ee0` → `8cbea2a`,
branch `bug/recursion`). **Turbo works and is organic-safe** (TCP `turbo enabled=1`
→ ~2.4× block-dispatch rate; the freeze is identical, just reached faster).

- **Run #1** (frame 63826, `recursion_func 0x8004DEE0`): the edge I instrumented,
  **interp→`0x8001A954`, is ORDINARY** — exactly **1/frame, every frame, even on the
  crash frame** (`max_per_frame=1`). The cycle counter advances normally per frame.
  So the 23k-deep recursion is NOT that edge.
- **Run #2** (frame 49599, `recursion_func 0x8005FF60`): `dispatch_8001A954` =
  **`max_per_frame=0`, `last_frame=0xFFFFFFFF`** → `dirty_ram_dispatch` was **NEVER
  called with `0x8001A954`**. So `func_8001A954` isn't re-dispatched via
  `dirty_ram_dispatch` either. And **`recursion_func` VARIES** (0x8004DEE0 /
  0x8004DFA0 / 0x8005FF60). => **address-specific counters are the WRONG instrument**;
  the recursing functions change run to run.

**The data-driven cycle (from `dirty_block_tail`, run #2, frame 49599) is the real
picture:** `dirty_ram_dispatch` targets are **overlay blocks `0x8011xxxx`–`0x8013xxxx`
(+`0x800E9xxx`)**, called (the `ra` field) from **main-EXE `0x8002xxxx`–`0x8005xxxx`**
(the `0x8005FF60` cluster). Repeating targets (`0x8012608C`×4, `0x800E9120`×4). So the
runaway is the **per-frame MESSAGE/RENDER overlay system** (main-EXE message handlers
`0x8004–0x8005` ↔ interpreted overlays `0x8011–0x8013`), and *which* functions recurse
depends on which message/state is rendering when it trips. The `func_8001A954` chain
from the minidump (§1) was just one instance of this family.

**BUG fixed:** the 3 site recorders (256 entries × 3) overflowed `crash_trace.c`'s 8KB
`re954[]` sub-buffer → truncated the whole report JSON. `SITE_CAP` 256→32 (committed)
so reports parse. **But the recorders are at the wrong sites anyway.**

**PIVOT for next session — stop guessing addresses:**
1. Re-soak with the fixed buffer (turbo'd) and read the **existing data-driven rings
   from a clean report**: `native_stack` (the host cycle), `dirty_block_tail` (the
   `dirty_ram_dispatch` `target`+`ra` cycle — the `ra` names the re-dispatch caller),
   `dispatch_tail`. These show the ACTUAL cycle without guessing.
2. From the `ra` callers (main-EXE `0x8002–0x8005`), Ghidra those functions to find
   the per-frame loop and its **termination condition**.
3. Add the **last-writer map** (hook `psx_write_word/byte/half` in memory.c — generated
   code writes via `cpu->write_word`→these; writer-PC via... NOTE `g_debug_last_store_pc`
   is NOT set per-store in generated code, count was 0 — need another writer-PC source,
   e.g. a global cpu via `debug_cpu_ptr` (memory.c:252) + its pc) on the controlling
   value → the causal slice "loop didn't terminate because reg==V from addr Y written
   by PC Z." `psx_read_word/half/byte` (memory.c 586/671/727) are free funcs usable in
   the write hook to read state.
4. Determinism check worth doing: does the freeze frame VARY run-to-run (63826 vs 49599)
   or is it input/navigation-dependent? Both runs idled on a dialogue; frames differ.

---

## 14. Fix attempt + CORRECTED understanding (session 2, 2026-06-17 PM)

Implemented + live-tested the single-mixed-dispatch-owner fix (§11) at the interp↔
compiled boundary (commits `f488dc5` → `e59043b`, branch `bug/recursion`). Two
iterations: a nesting-counter trigger that **LEAKED** across the fiber exception
longjmp (`psx_exception_longjmp` never returns → the post-call decrement is skipped
→ the counter crept up → false-tripped on a benign call → froze a render) was
replaced by a host-stack-**USAGE** watermark (`PSX_MIXED_STACK_KB`, default 700; can't
leak). Gated by `PSX_MIXED_OWNER` (default on).

**RESULT — the fix is a SYMPTOM-fix and does NOT make the game playable.** Live 2×2
soak (A/B fix-on, C/D fix-off, `overlay_cache` OFF): **ALL FOUR froze at ~14 min
(~frame 50k), fix on or off.** C/D overflowed (recursion crash, harvest report); A/B
hung "(Not Responding)" (the watermark surfaced into the still-wedged guest state and
the main thread spun). The fix bounds the host stack (no overflow crash) but the guest
still wedges → hang — arguably *worse* for debugging than the crash-with-report.
Keep it gated; default it OFF in a future pass.

**CORRECTION — strike a wrong mid-session assessment.** The freeze is **NOT
dialogue-triggered.** It is the **~14-minute IDLE freeze**, time/frame-based
(~frame 50k), **location-independent** (New Game area, Dwarf Forest, or idling on a
dialogue — all identical; same conclusion we reached for Dwarf Forest earlier). The
screenshots of a frozen dialogue were just *where the game sat idle*. The
`e59043b` commit message claiming it "fires at the first dialogue" is **WRONG**:
`overlay_cache` OFF does not move the trigger earlier — the ~14-min timing is the SAME
with cache on or off, so the trigger is NOT interp-coverage; it is a time/frame-
correlated divergence. (USER correction, authoritative.)

**Root, restated:** at ~frame 50k of idle, our sim diverges from real hardware — which
idles indefinitely, **USER-CONFIRMED** — and tips the per-frame loop
(`func_8001A954` → `func_80046264`) into the boundary recursion. The recursion is the
SYMPTOM; the time-based divergence is the ROOT. Every soak so far was **us-vs-us**; we
have never diffed against real hardware over the idle window.

**NEXT = the oracle.** psxref (`F:\Projects\psxref`, Beetle PSX, TCP **4380**, shares
`card1.mcd`; see [[psxref_oracle]] / `memory/psxref_oracle.md`). Run BOTH our recomp
and psxref idle (**turbo** if possible, to reach ~frame 50k in well under 14 min wall
clock) and find the **FIRST** state / control-flow divergence around frame ~50k — that
divergence is the bug. Candidate signals to diff (find_first_divergence, emu_read_ram,
emu_trace_addr, emu_step): the guest frame/timer counters, the state object
`*(0x1F8001D4)+0x4A`, the dirty-bitmap growth, IRQ `I_STAT`/`I_MASK`, root-counter
state. Whatever first differs from real HW is the lead. The host-stack fix is a dead
end for *fixing* it (kept only as a robustness/observability aid).

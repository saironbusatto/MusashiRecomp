# Handoff — MMX6 first-divergence co-sim oracle (2026-06-30)

Continuation of the MMX6 cutscene→gameplay wedge hunt. Read `COSIM_ORACLE.md` first
(the design + the four validation gates). This file = current state + exact next steps.

## The mandate (do not drift from this)

After ~2 weeks of confident-but-wrong "this is it" root causes, the user forbade chasing
any more single hypotheses — **including** the currently-leading-but-UNPROVEN
GPU-draw-time-backpressure idea (Beetle-anchored, bisect-confirmed as a *lever*, never
rendered a frame). We are building a **decision procedure**: a full-architectural-state
lockstep co-sim of the two backends that halts at the FIRST divergence. Whatever it halts
on *is* the first divergence — no hypothesis can be "wrong." It will confirm or kill GPU
backpressure (and everything else) on its own. **Do not implement GPU backpressure or any
other fix until the oracle names the first divergence.**

## Established facts (proven; do NOT re-chase — each cost days)

- Compiled backend SKIPS the intro FMV (main-state 0→1, no substate 6) → wedges black at
  `02 02`. Interp (`PSX_FORCE_INTERP=1`) plays it. SAME binary.
- Given identical inputs, compiled == interp byte-for-byte (block+function lockstep, 12.9M
  + 560K units, zero divergence). So it's NOT codegen — it's WHEN an event is taken
  relative to the instruction stream, which flips an early control-flow branch.
- Ruled out with data: async-RFE, CD-DMA-0x79D44 corruption (interp does the same write),
  IRQ-storm/timeline, block-edge-vs-per-insn IRQ delivery, load-delay-slot exception
  corruption (built + refuted, reverted).
- `PSX_LOAD_DELAY=0` makes compiled play the FMV (pixels) — but that's a secondary phase
  modulator, not the faithful root, and it does NOT fix the later `02 02` gameplay wedge.
- TWO separate bugs: (1) FMV-skip, (2) `02 02` gameplay wedge (persists even with FMV fixed).

## What is BUILT (this session, all uncommitted on branch `codex/mmx6-precise-slice`)

Worktree: `F:\Projects\psxrecomp\_wt-tomba2\psxrecomp`. All cosim code is `#ifdef PSX_COSIM`
(zero effect on normal builds).

New files:
- `COSIM_ORACLE.md` — design + gates (READ FIRST).
- `runtime/include/cosim_state.h`, `runtime/src/cosim_state.c` — full guest-arch-state
  canonical FNV hash. Reuses `boot_state.c` per-device `_snapshot_write` serializers; ADDS
  the CPU micro-state boot_state omits (muldiv_ts_done, gte_ts_done, read_absorb[33],
  read_absorb_which, read_fudge, ld_which_t, ld_absorb). Incremental RAM page-hashes via
  write hooks. VRAM excluded v1 (downstream, not causal). Gate-4 fault injection built in.
- `runtime/src/cosim.c` — engine: CYCLE-KEYED checkpoints (ring + cumulative chain hash +
  park/step lockstep) + standalone minimal TCP server. Commands: status, chain, stride N,
  runto <cycle>, hash, sub, window N, inject ram|reg, reset.
- `tools/cosim.py` — coordinator: launches 2 instances, cycle-locksteps via runto/chain,
  reports first divergence + sub-hash diff + window. Its --help documents the gate runs.

Modified (guarded):
- `recompiler/src/code_generator.cpp` + `full_function_emitter.cpp` — emit guarded
  `cosim_block(pc)` at each block leader (+ extern decls). REQUIRES REGEN to take effect
  (see below); the CORE compare does NOT need it (see "why regen is optional for v1").
- `runtime/src/psx_cycles.c` — `cosim_tick()` in `psx_advance_cycles` (THE alignment clock:
  the only counter both backends drive identically).
- `runtime/src/dirty_ram_interp.c` — `cosim_block(pc)` in the interp loop.
- `runtime/src/memory.c` — `cosim_note_ram_write` in word/half/byte RAM writes (covers
  CPU + DMA; DMA verified to route through psx_write_*_raw via the d44_ring evidence).
- `runtime/src/interrupts.c` — `interrupts_cosim_hash()` (folds cycles_since_vblank +
  in_exception ONLY; EXCLUDES total_checks/dispatch_count/cooldown = backend-call-freq
  artifacts that would false-positive). ALSO: reverted the refuted load-delay save/restore.
- `runtime/src/main.cpp` — `cosim_init()` at startup.
- `runtime/runtime.cmake` — cosim sources added to shared list; `COSIM` target option →
  `PSX_COSIM=1 PSX_NO_DEBUG_TOOLS=1`.
- `MegaManX6Recomp/CMakeLists.txt` — `psx-cosim` target gated on `-DPSX_BUILD_COSIM=ON`.

Also still uncommitted from earlier (Codex): the precise-slice / `--headless` mode / etc.
(both emitters have `psx_precise_interrupt_at`). Leave as-is; irrelevant to the oracle.

## Build state (IN PROGRESS at handoff)

- `build-wt` CANNOT be reconfigured (pre-existing worktree cache-path error, NOT our
  changes). So the cosim build uses a FRESH dir:
  `F:\Projects\psxrecomp\MegaManX6Recomp\build-cosim` (configured OK with:
  `-G Ninja -DPSXRECOMP_V4_ROOT=F:/Projects/psxrecomp/_wt-tomba2/psxrecomp
   -DPSX_BUILD_COSIM=ON -DPSX_LAUNCHER=OFF` + the mingw64 gcc/g++/ninja from build-wt cache,
  RelWithDebInfo).
- BUILD PROGRESS: ALL translation units compiled CLEAN under PSX_NO_DEBUG_TOOLS+PSX_COSIM
  (no unguarded-debug-call errors — the guards are correct). The ONLY failure was one link
  error: `cosim_init()` called from main.cpp (C++) needed `extern "C"` — FIXED
  (main.cpp:2486). Relinking at handoff. Expect the exe to link now; if any NEW link error
  appears it'll be another C/C++ linkage or a missing cosim symbol — all cosim funcs are in
  cosim.c/cosim_state.c (C files, added to the shared source list) so they should resolve.
- Build/run commands:
  Configure (once):
    cmake -S F:/Projects/psxrecomp/MegaManX6Recomp -B F:/Projects/psxrecomp/MegaManX6Recomp/build-cosim \
      -G Ninja -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe \
      -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe \
      -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DPSXRECOMP_V4_ROOT=F:/Projects/psxrecomp/_wt-tomba2/psxrecomp \
      -DPSX_BUILD_COSIM=ON -DPSX_LAUNCHER=OFF
  Build:  cmake --build F:/Projects/psxrecomp/MegaManX6Recomp/build-cosim --target psx-cosim -j 4
  (exe at build-cosim/psx-cosim.exe; run coordinator: python tools/cosim.py --a compiled --b compiled)

## MILESTONE (2026-06-30 late): ORACLE BUILDS + RUNS + EARLY BOOT DETERMINISTIC

`psx-cosim.exe` links (build-cosim, 89MB). Coordinator smoke test
(`--a compiled --b compiled --stride 262144 --max 8000000`, ~14 frames early boot):
BOTH instances launch, connect (4600/4601), cycle-lockstep via `runto`, and report
**ZERO divergence** — chains match at every checkpoint. So the full mechanism works:
two-process launch, TCP, cycle-stepping, full-state hashing, chain compare. Early boot
is deterministic. Deeper compiled-vs-compiled (to 250M cyc / ~440 frames) running to test
overlay-load-timing determinism (result in build task b5lvwwi0i / next session).

## HARNESS BUG FOUND + FIXED (the gates working as intended, 2026-06-30)

Two bugs the validation caught before any real result was trusted:
1. Coordinator `kv()` parser misaligned the `runto` reply (leading bare word `parked`),
   so `chain` parsed as None for BOTH sides → every compare was None==None == "equal" →
   the tool was SILENTLY BLIND (would report "no divergence" for a real one). Fixed
   (parse all adjacent token pairs) + added a FATAL guard if chain is ever unparseable.
2. With the parser fixed, gate-4 immediately exposed a REAL harness bug: the two
   processes parked at DIFFERENT cycles for the same `runto` target (A=3.07M, B=4.03M) —
   a racy design where the guest free-runs from launch and only notices the async stop
   flag at a wall-time-dependent moment. `runto <cycle>` did not stop AT that cycle.
   FIX (redesign): the guest now parks at EVERY checkpoint boundary (a deterministic
   cycle = multiple of stride) and only advances when the coordinator grants budget via
   `step N`. Stride is fixed at LAUNCH via env PSX_COSIM_STRIDE (no set-stride race —
   the guest hits cp=1 before the coordinator can send a command). Protocol is now
   `step N` (not `runto`); coordinator steps both 1 checkpoint at a time and compares
   the cumulative chain, with a cycle-skew WARN if the two ever park at different cycles.
   REBUILDING psx-cosim with this fix at handoff (task bxuiye5u3).

GATE 4 PASSES (post-fix, verified 2026-06-30): injected `ram:1000:255` into B before
cp 20 → tool HALTED at cp 21 (exact checkpoint after the fault), BOTH processes at the
IDENTICAL cycle 5242880 (deterministic park — race fixed, no skew warning), and the ONLY
differing subsystem hash was `ram` (cpu/irqctl/timers/clock/all devices identical). So the
tool provably: detects divergence, halts at the right checkpoint, parks deterministically,
localizes the subsystem. cp 1–19 identical before injection = partial Gate 1 passing.
GATE 1 PASSES (compiled-vs-compiled, no inject): 457 checkpoints to ~210 frames, ZERO
divergence, chains matched throughout — INCLUDING past overlay-load territory (frame
118+). So determinism + hashing + host-state exclusion are all correct, and overlay
loading is deterministic in compiled-vs-compiled (the warming concern did not bite).

==> TOOL VALIDATED (Gate 1 + Gate 4 both pass). THE compiled-vs-interp RUN is now running
(task bhi8hf5p1): `python tools/cosim.py --a compiled --b interp --stride 262144
--max 1000000000` (~frame 1766, past the FMV-skip ~1650). Interp is ~5 frames/sec so this
takes ~6-10 min. When it halts: the printed cp/cycle = the first divergence; the FIRST
differing subsystem hash (cpu / irqctl / a device / ram) = WHERE it split; the window =
the last 16 checkpoints of both. THAT names the bug — read it, then design the faithful
fix for whatever it actually is (could confirm GPU-backpressure, or something else).
If it shows a cycle-skew WARN, the two backends charge cycles differently somewhere (would
be its own finding). If interp is too slow to reach 1650, use a coarser --stride for a
first bracket then drill finer.

## OVERLAY CACHE DECISION (user, correct): leave `overlay_cache=true` — the gcc native
overlay shards ARE the compiled path we must measure; forcing them off would make the
"compiled" instance interpret overlays. The gcc compile worker only fires on a cache
MISS, so **do a WARMING RUN first** (free-run one psx-cosim instance through the FMV
region so autocompile populates build-cosim/cache), then every cosim run loads the shards
synchronously + deterministically (worker never fires). NOTE: at 14 frames no cache dir
existed yet (overlays load later in boot). OPEN: verify the autocompile cmd resolves from
build-cosim (game.toml cmd uses `../psxrecomp/...` relative → resolves to the MAIN checkout
F:/Projects/psxrecomp/psxrecomp, NOT the worktree; may need the worktree recompiler path or
a pre-populated cache). If autocompile can't run, overlays fall to dirty-interp in BOTH
compiled instances (still deterministic, but "compiled" won't exercise native overlays).
The Gate-1 deep run will REVEAL whether cold-cache overlay loading is nondeterministic.

## ★ FIRST RESULT (2026-06-30): oracle HALTED at cp=32, cycle 8,126,465 (frame ~14)

compiled-vs-interp first divergence is at frame ~14 (EARLY BIOS BOOT, long before the FMV
skip ~frame 1650). Both at the IDENTICAL cycle (no skew). 31 checkpoints matched exactly,
then cp32 splits. The ONLY differing subsystem is `cpu` — ram, scratch, gpu, spu, cdrom,
dma, sio, timers, clock, dirty ALL identical. `istat=00000001` (VBLANK pending) at the
split; interp's last leader = 0x80054240 (BIOS shell). A CPU-only divergence with memory
+ devices identical and an IRQ pending is the SIGNATURE of an interrupt taken at a
different instruction between backends — the exact "when is the event taken" mechanism,
caught at its FIRST occurrence. BUT NOT YET CONFIRMED: need to know WHICH cpu field split.
Added a `cpu` TCP command (full gpr/pc/cop0/micro-state dump) + coordinator
`--cpudiff-at-cp N`. REBUILDING (task b5y3113cg). Next: run
`python tools/cosim.py --a compiled --b interp --stride 262144 --cpudiff-at-cp 32` and read
which field differs:
  - pc / cop0[EPC=14] / cop0[SR=12] / cop0[Cause=13] differ => REAL: an IRQ was taken in
    one backend and not the other at this instruction. THIS is the root mechanism. Chase
    why (the accurate-timing IRQ take-point), fix faithfully.
  - ONLY a lone micro-state field (read_fudge/read_absorb_which/ld_which_t/ld_absorb/
    muldiv_ts_done/gte_ts_done) differs, with pc+cop0+gpr identical => the divergence is a
    benign load-delay/stall MICRO-STATE difference that hasn't affected architectural state.
    Decide: is it a real cycle-model mismatch (compiled vs interp charge differently here)
    — investigate psx_cyc_* ordering — or should that field be excluded from the hash as
    non-architectural (refine per COSIM_ORACLE.md). Then continue toward frame 1650.
  - a gpr differs with pc identical => a real data computation divergence at this PC
    (contradicts the lockstep proof — investigate that instruction).
CAVEAT: `pc` in the ring is g_last_leader_pc, only set by cosim_block; WITHOUT REGEN the
compiled side never calls cosim_block for game/BIOS text (only the interp loop does), so
the compiled ring `pc` is STALE/unreliable for labeling. The `cpu` command reads the REAL
debug_cpu_ptr->pc, which IS reliable. (Regen game+BIOS to make ring pc labels meaningful.)

## ★ cp32 RESOLVED: it was `pc` currency, a FALSE POSITIVE (2026-06-30)

CPU field-diff at cp32: the ONLY differing field is `pc` (compiled=0x00000000,
interp=0x80054240). EVERY gpr/cop0/hi/lo/micro-state IDENTICAL. Root: the compiled
backend does not keep cpu->pc current mid-execution (writes it only at block transfers;
transiently 0 between dispatches), while interp keeps it exact per-instruction — so at a
mid-instruction cycle checkpoint they hold different pc while in the SAME architectural
state. FIX: excluded cpu->pc from the cross-backend hash (cosim_state.c hash_cpu; still
reported via the `cpu` command). NOT a blind spot — a real control-flow split shows as a
differing gpr/memory value within one checkpoint (stride 262144 = ~50k instructions).
Gate 1 (compiled-vs-compiled) could not catch this because both sides share pc-currency.
REBUILDING (task b2n8aw3v0). This is the tool honing itself exactly as the gates intend:
it surfaced a representational difference at a clean bracketed point with the exact field
named, instead of letting it corrupt a result.

## ★★ SECOND RESULT (pc excluded): first REAL divergence at cp=42, frame ~18

Re-run compiled-vs-interp with pc excluded: CPU now MATCHES (pc fix worked), tool stepped
past cp32 and halted at cp=42, cycle 10,747,916 (frame ~18), same cycle both sides. The
differing subsystems are `irqctl` AND `gpu` — `cpu`, `ram`, `scratch`, `clock`, `timers`,
cdrom, dma, sio, spu ALL identical. This is DEVICE/INTERRUPT-TIMING state, not CPU/data —
cannot be a pc-currency artifact (both drive devices via the same psx_advance_cycles).
`irqctl` = cycles_since_vblank + in_exception + i_mask (+ i_stat, which the window shows =1
on both). HYPOTHESIS (unconfirmed, must field-check): VBLANK is raised at a different guest
cycle because compiled checks interrupts at BLOCK edges while interp checks per-instruction
→ cycles_since_vblank diverges AND gpu_vblank_tick (GPUSTAT LCF) diverges → both irqctl+gpu
split. That IS the "when is the event taken" mechanism, at its first real occurrence.
CONFIRM: added `dev` TCP cmd (csv=cycles_since_vblank, inexc, imask, istat, gpustat) +
coordinator prints DEV field-diff in --cpudiff-at-cp mode. REBUILDING (task b8nhp6ub1).
Run: `python tools/cosim.py --a compiled --b interp --stride 262144 --cpudiff-at-cp 42`.
  - dev.csv differs => CONFIRMED VBLANK-raise-timing divergence (interrupt-check frequency
    differs between backends). The faithful fix: raise VBLANK (and all scheduled device
    events) at their exact due guest-cycle in BOTH backends, independent of how often
    psx_check_interrupts is called — i.e. a due-cycle event scheduler, not a check-at-
    block-leader model. (This is the class fix CLAUDE.md/FAITHFUL_TIMING_PLAN point at.)
  - dev.gpustat differs but csv same => a GPU-internal divergence (could be the draw-time /
    GPUSTAT-ready model = the GPU-backpressure hypothesis, NOW testable at this exact point).
  - Add per-field device dumps as needed to pin the exact GPU field.

## ★★★ CONFIRMED (2026-06-30): first divergence = VBLANK raised at a different guest cycle

DEV field-diff at cp42 (cycle 10,747,916, frame ~18):
  dev.csv (cycles_since_vblank): compiled=0x590c (22796)  interp=0x8f60c (587788)
  dev.gpustat:                   compiled=0x94800000       interp=0x14800000  (bit31 LCF differs)
VBLANK_CYCLES=564480. Compiled's last VBLANK was ~22796 cyc ago; interp is at 587788
(PAST the threshold, next VBLANK not yet fired). Their last-VBLANK cycles differ by
~564992 ≈ ONE VBLANK period → compiled has raised ONE MORE VBLANK than interp at the same
guest cycle; GPUSTAT LCF (bit31, toggled by gpu_vblank_tick) confirms. So the FIRST real
compiled-vs-interp divergence is EVENT-PHASE: VBLANK is raised at a different guest cycle
between backends. This is the measured root of the "when is the event taken" class; the
FMV skip 1600 frames later cascades from it. cpu.pc also shows (A=0 transient, excluded).

WHY (next investigation — cycles_since_vblank increments identically via psx_advance_cycles
in both; it only RESETS inside psx_check_interrupts when cycles_since_vblank>=VBLANK_CYCLES):
whichever backend calls psx_check_interrupts first after crossing the threshold fires+resets
first. Candidates: (a) psx_check_interrupts call FREQUENCY differs (compiled at block leaders
vs interp per-instruction) so one crosses/fires at a different cycle; (b) the VBLANK DEFER
logic (interrupts.c ~line 388: defer while sio_card_protocol_active() / progress_stale)
evaluates differently between backends. SIO sub-hash matched at cp42, so lean toward (a).
FAITHFUL FIX DIRECTION: raise VBLANK (and all scheduled device events) at their EXACT due
guest-cycle independent of psx_check_interrupts call frequency — a due-cycle event scheduler
(FAITHFUL_TIMING_PLAN.md). CONFER ChatGPT before implementing. Then rebuild psx-cosim,
re-run compiled-vs-interp: the first divergence should move much later; iterate.

## NEXT (immediate): re-run compiled-vs-interp with pc excluded
`python tools/cosim.py --a compiled --b interp --stride 262144 --max 1000000000`
Expected: proceeds well past cp32 toward the FMV-skip (~frame 1650 / ~933M cyc). Outcomes:
  - Halts near frame 1650 with a CPU (cop0/gpr) or device subsystem diff => THE bug's first
    divergence. Use `--cpudiff-at-cp N` (or add device field dumps) to name the exact field.
  - Halts EARLIER on another currency artifact => field-diff it; if it's another
    mid-instruction-currency field (unlikely — only pc had this), exclude/normalize similarly.
  - Reaches frame 1650 with NO divergence => the FMV bug is NOT in the guest-architectural
    state the hash covers at this stride; drill finer, or the divergence is in a device
    internal not yet in the snapshot (audit boot_state device _snapshot completeness).

## NEXT STEPS (in order)

1. Finish the `psx-cosim` build; guard any `PSX_NO_DEBUG_TOOLS` link/compile errors.
2. **Why regen is optional for v1:** the compare is the cycle-keyed full-state hash in
   `psx_advance_cycles` (works without regen). `cosim_block` only supplies a human-readable
   last-leader-PC for the report. So you can run the gates + first compiled-vs-interp pass
   on the CURRENT generated C, then regen (game+BIOS) later for nicer PC labels.
3. **RUN THE GATES (do not skip — this is the whole point):**
   - Gate 1: `python tools/cosim.py --a compiled --b compiled` → MUST report zero
     divergence across boot. If it diverges, the clean build still has NONDETERMINISM
     (threads/host-time) OR the hash includes host-only state — fix that FIRST. Likely
     suspects: SPU worker thread, overlay compile worker, any wall-clock in the guest path,
     autocompile. The clean build should be single-threaded; verify no worker threads run.
   - Gate 1b: `--a interp --b interp` → zero divergence.
   - Gate 4: `--a compiled --b compiled --inject-at <cyc> --inject ram:100000:1` → MUST
     halt at ~that cycle. If it says "no divergence," the tool is BLIND — fix before trusting.
4. **THE RUN:** `python tools/cosim.py --a compiled --b interp` → first divergence =
   the bug. Read the `sub` diff (which subsystem's hash split first) + `window` (last N
   checkpoints of both). That names it: cpu / irqctl / a device / RAM address.
5. Only THEN design the faithful fix for whatever it actually is.

## Traps / notes

- Determinism is the load-bearing assumption (see COSIM_ORACLE.md "two instances"). Attract
  mode needs no input, which helps. Gate 1 PROVES determinism; don't believe any
  compiled-vs-interp result until Gate 1 + Gate 4 pass.
- Reading parked state must be side-effect-free (the device _snapshot_write serializers are
  reads; verify none mutate).
- Ports: A=4600, B=4601 (env PSX_COSIM_PORT). Coordinator sets BELOW_NORMAL priority.
- `runto` timeout in cosim.c is ~600k ms of 1ms spins; a very large `--stride`×`--max` at
  interp speed can be slow — interp is ~10× slower, budget accordingly.
- Pin/reference: `build-master/psx-runtime.PIN.exe` = the working (old-pin) MMX6. `build-wt`
  = the regressed accuracy build. Memory: `mmx6_regression_compiled_codegen.md`.

# Cycle-isolation micro-benchmark test ROM (ruler #2)

A purpose-built, in-tree, version-controlled cycle test program — the
**integration-test ecosystem** for the faithful R3000A cost model
(FAITHFUL_TIMING_PLAN.md §3c, ACCURACY_BURNDOWN.md axis 2). Complements ruler #1
(the BIOS-kernel function ruler) by isolating ONE cost component per loop, which
organic BIOS/game code can't (it mixes components in a single basic block).

This is game-independent: it validates the shared `psx_instr_base_cycles` +
memory-path model that EVERY recompiled PSX title uses, not just the current
bring-up vehicle.

## What it is

`gen_testrom.py` emits `cycle_testrom.exe`, a tiny PS-X EXE of counted loops.
Each loop's top is a basic-block leader (branch target), so a cyc_watch
single-anchor consecutive-hit Δ = exactly one iteration's cycle cost — the same
measurement on both backends.

Loops (each adds ONLY its op(s) over the shared loop overhead):
- `baseline`   — loop overhead only (addiu; bne; nop)
- `alu`        — + one addu (pure execute)
- `load`       — + one lw from main RAM (memory read wait-state)
- `load2`      — + two lw (linearity check)
- `div`        — + divu, mflo (div latency + stall-on-read, worst case)
- `div_spaced` — + divu, 2 filler addu, mflo (stall partly absorbed)
- `mult`       — + multu, mflo (mult latency)

**Isolation by baseline subtraction:** (component_per_iter − baseline_per_iter)
= the component's cost; loop overhead AND instruction fetch (both fully
cache-resident after warm-up) cancel.

## Two-backend measurement

Same EXE, both backends, compare per-iteration Δ:
- **native** (psx-runtime cost model): recompiled by `psxrecomp-game`; the game
  emitter emits `debug_server_cyc_observe` at every block leader (debug builds),
  so each anchor is observable. (Native delivery harness: see "Native side" TODO.)
- **Beetle** (HW oracle): mednafen sideloads the raw PS-EXE (it auto-detects the
  "PS-X EXE" header). cyc_watch the same anchors.

Each component is then transcribed/calibrated into `psx_instr_base_cycles`
(execute latencies: mult/div, GTE) or the memory-path wait-state (`memory.c`),
Δ-gated against Beetle on these loops + FMV-no-regression, one at a time.

## Boot disc (how the EXE reaches both backends)

The real BIOS only boots a disc that carries the PlayStation license region; a
license-less disc drops to the BIOS shell (Memory-Card/CD-Player menu). So the
test EXE ships on a synthetic disc built with the in-tree `tools/mkpsxiso`:

```bash
cd tools/cycle_testrom
python gen_testrom.py cycle_testrom.exe                    # EXE + .anchors.json
../../recompiler/build-t2/psxrecomp-game.exe --config game.toml   # recompile (native)

# ONE-TIME: extract the license region from a disc YOU OWN (local only, never
# redistributed). dumpsxiso writes license_data.dat from the disc system area:
DUMP=../mkpsxiso/mkpsxiso-2.20-win64/dumpsxiso.exe
"$DUMP" -x /tmp/own_disc -s /tmp/own_disc/x.xml "<path to a PS1 disc you own>.cue"
cp /tmp/own_disc/license_data.dat disc/license_data.dat    # gitignored, stays local

# build the disc (embeds license + SYSTEM.CNF + cyctest.exe):
cd disc && ../../mkpsxiso/mkpsxiso-2.20-win64/mkpsxiso.exe -y cyctest.xml
```

`disc/license_data.dat` and the built `disc/*.bin`/`*.cue` are **gitignored** —
copyrighted Sony data, local only. Everything else (gen, xml, SYSTEM.CNF) is
tracked, so each developer reproduces the disc from a disc they own.

## Measure

```bash
# both backends boot the same disc/cyctest.cue:
#   beetle: psx-beetle <bios> --disc disc/cyctest.cue --port 4382
#   native: <test-runtime> ... --disc disc/cyctest.cue   (TODO: test-runtime)
python measure.py --port 4382      # Beetle oracle: per-iter + per-component delta
python measure.py --port 4500      # native cost model (once the test-runtime exists)
```

## Beetle ORACLE results (2026-06-26 — the HW cost targets)

Per-iteration cycle delta, and component cost (minus baseline=3):

| loop        | per-iter | component | reads as |
|-------------|----------|-----------|----------|
| baseline    | 3        | —         | addiu+bne+nop, base 1 each |
| alu         | 4        | +1        | one addu (pure execute) |
| load        | 8        | +5        | one main-RAM lw (result unused → no absorb) |
| load2       | 14       | +11       | 2 lw: 5 + 6 (2nd load's +1 = ReadFudge) |
| div         | 41       | +38       | divu+mflo base 2 + ~36 div stall |
| div_spaced  | 41       | +38       | divu+2addu+mflo — fillers ABSORBED by stall |
| mult        | 18       | +15       | multu+mflo base 2 + ~13 mult stall |

Key targets for the native cost model: **div stall ~36, mult stall ~13** (native
charges 0). Load ~5/6 (native memory.c flat +6 — close). **`div_spaced`==`div`
proves the stall must be modeled as a completion-timestamp + stall-on-mflo-read
(fillers absorb it), NOT a flat charge on the divu.**

## Status / TODO

- [x] Generator + hand-encoded MIPS isolation loops; encoding validated by a
      clean recompile (divu→`gpr[10]/gpr[11]`, etc.).
- [x] Universal per-block-leader `debug_server_cyc_observe` in BOTH emitters
      (BIOS `full_function_emitter.cpp` + game `code_generator.cpp`).
- [ ] Native delivery: a test-runtime target (link the generated C) that reaches
      `entry_pc` and free-runs the loops while the debug server serves cyc_watch.
      The boot→entry handoff (boot_state.c snapshot is disc-keyed) is the one
      piece to solve — options: a minimal boot disc, or a direct-entry harness.
- [ ] Beetle delivery: extend `beetle_main.cpp` to sideload a `.exe` content path.
- [ ] `compare.py`: arm each anchor on both backends, report per-component Δ vs
      the analytic expectation.

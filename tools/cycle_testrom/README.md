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

## Regenerate

```bash
cd tools/cycle_testrom
python gen_testrom.py cycle_testrom.exe        # writes the EXE + .anchors.json
# recompile for native:
../../recompiler/build-t2/psxrecomp-game.exe --config game.toml
```

Tracked: `gen_testrom.py`, `game.toml`, `seeds.txt`, this README.
Ignored (regenerable): `cycle_testrom.exe`, `*.anchors.json`, `generated/`.

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

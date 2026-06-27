# Load ReadFudge / LDAbsorb — empirically-derived model + implementation spec

Status: **MODEL NAILED (empirically, 2026-06-27), implementation PENDING.** This is
the last unmodeled piece of the load path — the residual on BOTH rulers
(ruler #1 native 54 vs Beetle 56; ruler #2 `load2` native +10 vs Beetle +11). It
is the R3000A load-delay pipeline interlock (Beetle `ReadFudge`/`ReadAbsorb[]`/
`LDAbsorb`/`LDWhich`). Derived by measuring Beetle's PER-INSTRUCTION cost via
adjacent-PC region cyc_watch (Beetle observes per-instruction; the native compiled
emitter observes only at block leaders, so native per-insn is not observable — Beetle
is the per-instruction oracle here).

## Empirical per-instruction costs (Beetle oracle, ruler #2 loops)

| loop      | per-instruction Δ (cycles)                  | per-iter |
|-----------|---------------------------------------------|----------|
| baseline  | addiu **1**, bne **1**, nop **1**           | 3        |
| load      | lw **7**, addiu **1**, bne **0**, nop **0** | 8 (+5)   |
| load2     | lw1 **7**, lw2 **6**, addiu **1**, bne **0**, nop **0** | 14 (+11) |

Reading these:
- **1st lw = 7**: fudge 2 + region 3 + completion 2. The +2 fudge appears because the
  instruction before it (nop / branch) committed NO load.
- **2nd lw = 6**: fudge **0** (prev instruction was a load) + region 3 + completion 2
  + **its own §1 base +1 (NOT absorbed)**. = 0+3+2+1.
- **delay-slot instruction (addiu) stays 1**: its §1 runs BEFORE its own DO_LDS sets
  `ReadAbsorb[loaded]`, so it cannot absorb the load it follows.
- **bne, nop = 0**: absorbed from the load's `LDAbsorb` give-back.

## The model (Beetle cpu.cpp, confirmed by the table above)

Per instruction, in order:
1. **fetch** (§2, separate axis — I-cache; 0 for cached-RAM steady state).
2. **§1 base** (cpu.cpp:795-798): `if (ReadAbsorb[ReadAbsorbWhich]) ReadAbsorb[ReadAbsorbWhich]--; else timestamp++;`
   — `ReadAbsorbWhich` = the GPR the most-recently-committed load wrote.
3. **GPR_DEP/RES** (cpu.cpp:702-705): for each source/dest reg of this instruction,
   `ReadAbsorb[reg]=0` (save/restore `ReadAbsorb[0]`). This ends the give-back when the
   loaded value is actually used (load-use hazard).
4. **execute**; a load (LW/LH/.../LWC2) runs ReadMemory (§3, cpu.cpp:364-451):
   - `timestamp += (ReadFudge>>4)&2` — the **fudge**. NOTE: `ReadFudge` holds the last
     load's dest reg (0-31) OR **0x20** when the previous instruction committed no load.
     `(x>>4)&2` is **0 for any reg 0-31** and **2 only for 0x20**. So fudge = **+2 iff the
     previous instruction committed no pending load**, else 0. (This is the non-obvious
     part — it is NOT per-register.)
   - region wait-state (main RAM = 3; scratchpad 0x1F800000-0x1F8003FF = 0 early-return;
     other regions differ), then `+2` (CPU load) or `+1` (LWC2/GTE load).
   - `LDAbsorb = lts - timestamp` = **region + completion (EXCLUDES fudge)** = 5 for main RAM.
5. **DO_LDS** (cpu.cpp:800), at the END of the handler: commits the PREVIOUS instruction's
   pending load: `GPR[LDWhich]=LDValue; ReadAbsorb[LDWhich]=LDAbsorb; ReadFudge=LDWhich;
   ReadAbsorbWhich |= LDWhich&0x1F; LDWhich=0x20;`. When `LDWhich==0x20` (no pending),
   `ReadFudge` becomes 0x20 → next load gets +2 fudge.

Stores are posted (+0, already modeled). Scratchpad loads are +0 (early-return, LDAbsorb=0).

## Why our current model is off

Current: flat `+1` base per instruction (PSX_CODEGEN_CYCLE_PER_INSN) + `memory.c`
charges a flat region wait-state (PSX_RAM_READ_WAIT_CYCLES=4) per RAM read. This
captures region+completion≈5 for an isolated load (matches `load`=+5) but MISSES:
(a) the **fudge** (+2 on a load whose predecessor wasn't a load → `load2` 2nd lw),
(b) the **absorb give-back** (following instructions going free), and
(c) scratchpad=+0. (a) and (b) happen to partially cancel on the `load` loop (why
single-load already matches) but not on `load2` / dependent-use sequences.

## Implementation plan (pervasive — a focused next task)

1. CPUState (append at end): `ReadFudge`, `ReadAbsorb[33]`, `ReadAbsorbWhich`,
   `LDWhich`, `LDAbsorb`. (`LDValue`/value-commit already handled by the existing
   load-delay-slot correctness path — only the TIMING state is new.)
2. `psx_cycles.c`: helpers for §1 (`psx_cyc_base`), the load cost+fudge+LDAbsorb arm,
   and DO_LDS timing-commit. region wait-state stays address-keyed in `memory.c` but
   must feed LDAbsorb (region+completion), and set fudge from ReadFudge.
3. Both emitters (code_generator.cpp, strict_translator.cpp) + dirty interp: replace the
   flat per-instruction `+1` with the §1 call, and emit `GPR_DEP/RES` zeroing for each
   instruction's source/dest registers (the pervasive part — every opcode passes its
   regs). Loads arm LDWhich/LDAbsorb; the next instruction's timing-DO_LDS commits.
4. Interaction with muldiv/GTE stalls: those use `*_ts_done` deadlines independent of
   ReadAbsorb — keep them; just ensure the §1 base replacement doesn't double-count.
5. **Validate** via the existing ruler-loop anchors (observable at loop tops): `baseline`=3,
   `load`=8, `load2`=14 must match Beetle EXACTLY; ruler #1 [c5c→ca4] should reach 56;
   FMV no-regression. Add a load-then-dependent-use loop to ruler #2 to pin the
   GPR_DEP give-back termination.

Risk: pervasive per-instruction emit change to the cost model — do it on a clean tree
with full budget, validate one ruler loop at a time.

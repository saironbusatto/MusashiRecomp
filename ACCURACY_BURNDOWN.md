# PSX Accuracy Burndown (living doc)

Companion to FAITHFUL_TIMING_PLAN.md. That doc owns the CYCLE/TIMING axis (the
area under active implementation); THIS doc is the full-coverage burndown across
ALL accuracy axes for the faithful-core build.

## Method (non-negotiable)

- Every item gets: **status**, the **external comparative(s)** to cross-reference
  it against, and a **validation method**. "Looks good" is NOT a status — an item
  is only GREEN once cross-referenced against a reference (psx-spx / Beetle source
  / DuckStation / a hardware test ROM) AND validated against the oracle at runtime.
  Self-agreement (compiled == our interp) proves backend-equivalence, NOT
  correctness — both can be identically wrong (CLAUDE.md §15).
- Don't do it all in one pass. Tomba 2 is the **stomping ground**: validate
  everything we can here, then validate the rest against the other games
  (Tomba 1, MMX6, Ape, BIOS), then merge wt/tomba2 → master, then keep a
  **post-merge burndown** for whatever remains.
- Governed by CLAUDE.md Rule -1 (faithful core, no hacks, breaking other titles OK).

## Comparative sources (the reference shelf)

- **nocash psx-spx** — the canonical hardware reference. Cite the section per item.
- **Beetle/Mednafen PSX** (IN-TREE: `psxrecomp/beetle-psx/mednafen/psx/`) — our
  oracle's own source: cpu.cpp, gte.cpp, gpu.cpp, spu.cpp, cdrom.cpp, dma.cpp,
  timer.cpp, frontio.cpp/`input/`, mdec.cpp, sio.cpp, spu/reverb. Read for the
  exact model; run as the oracle on port 4382 for runtime diff.
- **DuckStation** source — clean, heavily-commented; excellent cross-check for GTE
  cycle table, GPU rasterization rules, CD timing (use as a 2nd opinion vs Beetle).
- **Hardware test ROMs** (ground truth above emulators): Amidog CPU & GTE tests,
  PeterLemon/PSX, the psx-spx test suite, GPU/timing test ROMs. Running these on
  native vs Beetle is the strongest single validation we can add.
- **Real HW via Beetle oracle** (port 4382) — first-divergence on the relevant
  state surface (VRAM for GPU, audio samples for SPU, cycle counts for timing).

## Validation infrastructure to BUILD (prerequisite tooling)

- [ ] **Native↔Beetle cycle/first-divergence comparator** (replaces stale
  DuckStation-era find_divergence.py port 4371). Needs additive guest-cycle
  exposure in beetle_debug_server.c. The backbone measure for axes 2/3.
- [ ] **State-surface diffs**: VRAM byte diff (GPU), SPU sample-stream diff
  (audio), CD sector/response diff. Per-axis oracle comparators.
- [ ] **Hardware-test-ROM harness**: run Amidog/GTE/CPU test ROMs on native and
  Beetle, diff pass/fail + result registers. (Highest-leverage axis-1/2 validator.)

---

## Axis 1 — Instruction semantics (decoder)

Status: STRONG (recompiler core; proven byte-identical to interp on many funcs).
- [ ] ALU/shift/logical/sign-extension — cross-ref Amidog CPU test ROM.
- [ ] LWL/LWR/SWL/SWR unaligned — psx-spx "CPU Load/Store"; Amidog.
- [ ] MULT/MULTU/DIV/DIVU → HI/LO, div-by-zero & overflow results — psx-spx; Amidog.
- [ ] Overflow-trapping ADD/ADDI/SUB (vs ADDU/etc.) — do we trap? psx-spx "Exceptions".
- [ ] **Load-delay slot hazard** — KNOWN SIMPLIFICATION: interp loads land
  immediately (no 1-instruction delay). Cross-ref psx-spx "CPU pipeline"; Beetle
  LDAbsorb. Decide: model it or document why safe.
- [ ] Branch-delay slot (incl. branch-likely absence on R3000) — done; spot-check.
- [ ] COP0 (mtc0/mfc0/rfe, Status/Cause/EPC/BadVaddr) — psx-spx "COP0".
- [ ] GTE/COP2 math: fixed-point, saturation, FLAG register, all 30+ ops —
  cross-ref Amidog **GTE test ROM** (the definitive validator) + DuckStation gte.cpp.

## Axis 2 — Cycle/timing  ← ACTIVE (see FAITHFUL_TIMING_PLAN.md)

Status: Stage-1 (1 cycle/insn, single-source seam in place); Stage-2 in progress.
- [x] Single-source `psx_instr_base_cycles` seam (identity), both backends.
- [ ] Mult/div latency — Beetle cpu.cpp MULT_Tab/muldiv; psx-spx.
- [ ] GTE per-command cycles — Beetle gte.cpp GTE_Instruction; DuckStation; psx-spx.
- [ ] Memory wait-states by region (RAM/BIOS-ROM/scratchpad/MMIO) — Beetle
  ReadMemory/PSX_MemRead timing; psx-spx "Memory Control".
- [ ] Instruction-fetch / I-cache timing — Beetle ReadInstruction; psx-spx.
- [ ] Validation: native cumulative cycles == Beetle at convergence (needs the
  comparator above).

## Axis 3 — Interrupt / event timing

Status: PARTIAL.
- [ ] Device IRQ-raise timing (VBLANK scanline, timer overflow/target, DMA/CD
  completion) — tied to axis 2/5; psx-spx per device.
- [ ] **IRQ take-point** (HW = exact instruction; us = block edge) — the parked
  precise-slicing (PRECISE_IRQ_SLICE.md). Validate vs Beetle exc_ring.
- [ ] Exception entry record (EPC/Cause.ExcCode/BD/Status-stack) — currently uses
  a sentinel EPC; cross-ref psx-spx "Exceptions"; Beetle. Validate exc_ring match.

## Axis 4 — Memory map / MMIO

Status: MODERATE-STRONG (regions games use).
- [ ] KUSEG/KSEG0/KSEG1 mirroring, scratchpad, cache-isolation (IsC) — psx-spx.
- [ ] I/O register semantics: read-to-clear, write-1-ack (I_STAT), masking,
  unmapped/garbage reads — psx-spx "I/O Map"; Beetle memory.cpp.

## Axis 5 — Peripherals / devices  ← SUSPECTED WEAKEST (user flag)

Status: MIXED — "works for tested games," NOT edge-validated. Likely more gaps
than we think; the **hybrid-pad failure in Tomba is an axis-5 (SIO/controller)
bug**, not timing.
- [ ] **SIO / controllers / memcard**: DualShock config-mode handshake (0x43),
  analog vs digital pad ID, the **hybrid pad mode failure** (Tomba) — Beetle
  frontio/input + psx-spx "Controllers and Memory Cards". HIGH PRIORITY per user.
- [ ] **GPU**: GP0/GP1 command set, rasterization rules (top-left fill, dithering,
  semi-transparency modes, mask bit, texture windows, blending), VRAM-as-texture —
  cross-ref DuckStation gpu_*, Beetle gpu.cpp, GPU test ROMs; validate by VRAM diff.
- [ ] **SPU**: 24 voices, ADSR, pitch/sample-rate, reverb, volume sweeps, IRQ —
  Beetle spu.cpp; psx-spx "SPU"; validate by audio-sample diff.
- [ ] **CDROM**: command set, response timing, sector read (data/XA/CD-DA), seek,
  shell/lid — Beetle cdrom.cpp; psx-spx "CDROM"; validate by sector/response diff.
- [ ] **DMA**: all 7 channels, block/linked-list/chain modes, timing, DICR/DPCR —
  Beetle dma.cpp; psx-spx "DMA".
- [ ] **MDEC**: macroblock decode, IDCT, color conversion, RLE — Beetle mdec.cpp;
  validate FMV frame diff vs Beetle.
- [ ] **Timers (0/1/2)**: all clock sources (sysclk/dotclock/hblank/÷8), modes
  (target/overflow/reset/IRQ-repeat/one-shot), sync modes — Beetle timer.cpp;
  psx-spx "Timers".

## Axis 6 — Static-vs-dynamic fidelity (recompiler-unique)

Status: STRONG (most project effort lives here).
- [ ] Self-modifying / install-at-runtime code (dirty-RAM interp) — ongoing.
- [ ] Function discovery / dispatch completeness (no missed indirect/jump-table
  targets) — resolve all dispatch misses each run (Tomba2Recomp CLAUDE.md).
- [ ] Call/return contract + stack fidelity — the blue-screen/wedge class.
- [ ] Backend equivalence (compiled == interp) — necessary, not sufficient.

## Axis 7 — Determinism

Status: SOFT SPOT.
- [ ] Boot run-to-run variance observed (sometimes wedges, sometimes clean) —
  track down; faithfulness presupposes determinism.

---

## Phasing

1. NOW: cycle axis (axis 2) Stage-2 on Tomba 2 (FAITHFUL_TIMING_PLAN.md).
2. Build the comparator/test-ROM tooling (enables GREEN-ing items above).
3. Burn down axes here on Tomba 2 where validatable; axis 5 (esp. SIO/controller
   for the hybrid-pad bug) is the priority second front.
4. Validate cross-title (Tomba 1, MMX6, Ape, BIOS) before merge.
5. Merge wt/tomba2 → master; keep this doc as the post-merge burndown for the rest.

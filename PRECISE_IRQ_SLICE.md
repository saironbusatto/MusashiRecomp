# Precise interrupt-take-point fix — cycle-budgeted precise event slicing

Status: IN PROGRESS (Phase 1). Branch wt/tomba2. Root cause = interrupt
take-point granularity (compiled blocks take IRQs only at block edges; HW +
the dirty-RAM interpreter take them at the exact instruction). Oracle-confirmed
(Beetle exc_ring caught HW take at epc=0x8008593C, mid-block). Validated with
ChatGPT (Recomp project, "PSX IRQ Debugging" chat) — design below is the
agreed plan.

## Already in place (committed tip 8ab24c5, live)
- ABI v8 overlay callback `check_interrupts_at(cpu, resume_pc)`.
- Emitter emits `psx_check_interrupts_at(cpu, <resume_pc>)` at block EXITS
  (jr/jalr/branch/j/jal) — currently a runtime SHIM forwarding to
  `psx_check_interrupts` (interrupts.c). Block-LEADER `psx_check_interrupts`
  was removed.
- Per-block cycle accounting: `psx_advance_cycles(bcyc)` at each block leader
  (PSX_ENABLE_BLOCK_CYCLES=1). Fans out to sio/cdrom/dma/timers/interrupts
  _advance() in psx_cycles.c.
- Sanctioned dirty-RAM interpreter (dirty_ram_interp.c, complete MIPS+GTE+COP0)
  ALREADY takes IRQs at exact instructions, charges 1 cyc/insn, and is proven
  CYCLE+STATE identical to compiled for frames 1..1823. It is a trusted retire
  engine, not a fallback.

## The fix (3 mechanisms)

### 1. cycles_to_next_event(i_mask)  [Task #2 — additive, no behavior change]
Single source of truth = min over peripherals of the distance (guest cycles)
to the next DELIVERABLE interrupt raise (NOT merely the next i_stat raise).
"Deliverable" = the source's IRQ bit is unmasked in i_mask AND mode-enabled.
Each peripheral exposes its own `<p>_cycles_to_irq(uint32_t i_mask)` returning
UINT32_MAX if none:
- VBLANK: interrupts.c owns cycles_since_vblank -> VBLANK_CYCLES - cycles_since_vblank.
- timers: per timer with IRQ_TARGET/IRQ_OVERFLOW enabled AND bit in i_mask;
  ticks to next target/overflow * clk-divisor - timer_frac. Divisors: sysclk=1,
  timer1 hblank=2146, timer2 sysclk/8=8, timer0 dotclock=5.
- cdrom: min active countdown that raises bit2 (cdrom_irq_present_delay when a
  response is armed; pending.delay; read_delay) gated on bit2 in i_mask.
- dma: per active async channel, cycles to completion (remaining_words *
  CYCLES_PER_WORD - cycles_accum; advance_delayed_complete countdowns) gated on
  bit3 in i_mask + DICR enable.
- sio: sio_irq_countdown when armed + IRQ enabled, gated on bit7 in i_mask.
Conservative rule: when unsure, round DOWN (smaller distance => slice more =>
safe). Over-estimating the distance is the only dangerous direction.

### 2. Two-tier block execution  [Task #3]
At each block leader (emitter), BEFORE the body:
    fast-safe iff:  !irq_visible_now()
                 && cycles_to_next_event() > bcyc
                 && !block_has_irq_visibility_side_effects   (static, see below)
  fast-safe  -> psx_advance_cycles(bcyc); run compiled body (current path).
  else       -> cpu->pc = block_addr; return; and the dispatch loop enters
                PRECISE INTERPRETER MODE from block_addr.
`block_has_irq_visibility_side_effects` is a STATIC per-block emitter flag set
when the block writes I_STAT / I_MASK / COP0 Status/Cause / IRQ-source MMIO, or
contains mtc0 / rfe. Such blocks can change deliverability mid-block with no new
hardware event due, so they must take the precise path (or guard internally).

### 3. Precise interpreter mode  [Task #3]
Run the EXISTING dirty-RAM interpreter from cpu->pc. It CONTINUES ACROSS
fallthroughs, branches, jumps, jal/jalr, returns, compiled-function boundaries,
and dirty-RAM boundaries — owned by the EVENT DEADLINE, not the function
boundary (avoids the re-entrancy problem). Exit precise mode when ONE of:
  - the IRQ is taken (exact-instruction exception entry), OR
  - the event deadline passed and no IRQ is pending+enabled, OR
  - execution reaches a safe dispatch boundary where the next compiled block is
    provably fast-safe (cycles_to_next_event > that block's bcyc, no side effects).

### Exact exception entry  [Task #4 — interp already does most of this]
EPC = restart PC (branch PC if in a branch-delay slot; set Cause.BD), CP0
Status/Cause exact, load-delay carried into the handler. Do NOT manufacture a
block-entry EPC. Every IRQ-affecting write is an unconditional safe-point that
recomputes pending IRQ before the next instruction.

## Decisions locked with ChatGPT
- Reuse the interpreter as the precise sliced executor: SOUND and the better
  first implementation (1823-frame parity proves equivalence). Generated
  "precise sidecars" are only a later perf optimization, never a correctness need.
- Slice window owned by the event deadline; keep interpreting through callees.
- "check IRQ at block edge with EPC=block-entry/exit" is WRONG (fake EPCs) — the
  committed block-exit check is fine only as the fast path's own edge check;
  correctness comes from the leader slice-guard + precise mode.

## Validation  [Task #5]
- Beetle exc_ring oracle: native exception-entry record (cycle, last/next PC,
  EPC, BD, Status/Cause, I_STAT/I_MASK, pending-load) must match interp + Beetle
  at the f1823->1824 VBLANK.
- Regen + screenshot-smoke ALL titles (BIOS, Tomba 1, MMX6, Ape, Tomba 2).
- Delete Tomba2 overlay_native_block; native must still reach the FMV/title.
- Only then: pin bump (user-gated).

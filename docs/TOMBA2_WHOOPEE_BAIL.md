# Tomba 2 — Whoopee-Camp splash freeze (wild-call/return-contract bail storm)

Branch: `wt/tomba2`. Single source of truth for the splash freeze. Update as we learn.

## Symptom

Tomba 2 (SCUS-94454) boots to the Whoopee-Camp publisher splash and **freezes there**
— it never rolls into the intro FMV (the oracle, Beetle PSX on the same disc + BIOS,
sails past it to ~frame 270k). With the interrupt-pump fix (`0a6920a`) the process no
longer hard-deadlocks, but the splash never advances and the process degrades to a
Windows "Not Responding" state within ~2 minutes (the main thread is saturated).

## What it is NOT (ruled out with always-on heartbeat probes)

All of the hardware-sim is **healthy** at the spin — verified, not assumed
(probes survive the degradation because they are read from the heartbeat file):

- **Interrupts** (`75f1468`/`e1dce63`): VBLANK is **raised + delivered + ACKed exactly
  1/frame**; `irq_deliver` = 1/frame. Not an ack failure, not a delivery storm, not a
  re-entry storm (`exc_reentry` growth is just the one slow interpreted handler being
  polled, not actual nested entries).
- **Timer 1 / RootCounter 1**: free-run HBlank counter, `mode 0x1D00` (no IRQ
  configured = correct), `t1_count` advances, `t1_irq_fired = 0` is correct.
- **CD-ROM**: idle / Pause (correct — the splash hasn't issued the FMV load yet).
- **Kernel events (EvCB)**: snapshot at base `0xA000E028`, 16 entries, **all status =
  FREE** — no active event, so it is **not** a `WaitEvent`/`TestEvent` wait either.

## Root cause: wild-call/return-contract corruption at the exception ⟷ B0 boundary

The freeze is **not** an emulation bug — it is a **recompiler control-flow correctness
bug** in the wild-call/return-contract machinery (`psx_call_contract` in
`cpu_state.h`, and the generated `psx_dispatch_impl` trampoline). Same family as the
Bug A/C/D "wild call contract" freezes.

The contract bails when a callee C-returns but the guest state doesn't match the call
site: `cpu->gpr[31] != stop_addr` **or** `cpu->gpr[29] != sp_at_call`. On a bail it
sets `cpu->pc = $ra` and "flattens" at the outermost dispatch (re-dispatches the wild
target on a clean host stack).

Tomba 2's splash drives this **68,000 times per frame** (Tomba 1 at gameplay does
**zero** — so it is Tomba-2-specific, not a generic cost). Captured evidence:

- **Bail source** (runtime-only capture via `fntrace.c`, `f1aaedd`): dominant bailing
  function = **`0x000000B0`** (the **B0 kernel-syscall vector** / last compiled frame
  on the boundary).
- **Trigger** (first bail, frame **1823** = spin onset): `0xB0` → wild target
  **`0x8008AE50`**. Disassembly of the caller:
  ```
  0x8008AE48:  jal   0x80085900      ; call
  0x8008AE4C:  li    $a0, -1          ; delay slot
  0x8008AE50:  lui   $v1, 0x8010      ; <- jal+8 return address == the "wild target"
  ```
  So `0x8008AE50` is the **correct** return address of `jal 0x80085900` — the bail
  target is a *legitimate outer-frame return*, not a corrupt pointer. Both
  `0x80085900` and its caller are **high RAM (~0x80085000), OUTSIDE the recompiled
  game text** (`0x80010000`–`0x80038800`) → runtime-loaded / install-at-runtime code.
- **Register state at the spin** (clean snapshot):
  - `$ra = 0x00000001` — **corrupted** (degenerate dominant key has `$ra=$sp=1`).
  - `$sp = 0x801FFF48`, `s2 = 0x8008AE50` (the outer return, saved callee-side),
    `s3 = 0x8008ACFC`, `t1 = 0x35`.
  - `k0 = 0xBFC0193C` (**BIOS exception handler**), `epc = 0x80000048` (our EPC
    sentinel) → the spin runs in **exception-handler context**.

### Mechanism (current best model)

A VBLANK exception (delivered cleanly, 1/frame) fires while the high-RAM code is
**mid-call across the B0 / dirty-RAM boundary**. The exception delivery + resume/RFE
path does **not** correctly restore the interrupted frame's `$ra` (and, in the
degenerate steady state, `$sp`): the resumed code's `$ra` is clobbered to `1`. The
next return is therefore "wild" (`$ra != stop_addr`), the contract bails and flattens,
the flatten re-dispatches a wrong target, that corrupts state further, and it
**degenerates into an infinite bail loop** — the 68k-bails/frame splash spin.

This is consistent with the interrupt being *healthy at the controller level* (raised/
delivered/acked 1/frame) yet the *guest continuation* after RFE being corrupted — the
boundary between a delivered exception and an interrupted wild/dirty call.

## Open questions (for the fix)

1. Exact B0 function index at the trigger (`$t1` captured = `0x35` in the degenerate
   state; candidate `B0:0x17 ReturnFromException` fits the non-local-return shape —
   confirm by capturing `$t1` at the *first* bail, e.g. latch it in the heartbeat).
2. Precisely where `$ra` is clobbered to `1` — the exception push/RFE path
   (`interrupts.c` / `traps.c psx_syscall` ReturnFromException / the EPC-sentinel
   `0x80000048` handling) vs the bail-flatten re-dispatch.
3. Whether the first bail is itself a **false positive** (the contract wrongly flags a
   legitimate non-local return) or a **genuine** wild return whose *resume* is what
   corrupts state. The trigger target being a valid `jal+8` suggests the corruption is
   in the resume, not the detection.

## Fix direction

The exception/RFE resume across a wild/dirty call boundary must restore the
interrupted frame's `$ra`/`$sp` so the continuation returns to its real call site
instead of being flagged wild. Likely in the recompiler call-contract + the
exception-delivery/RFE path (never in generated C). Design before implementing — the
wild-call-contract family is subtle and has regressed under pressure before.

## Committed so far (`wt/tomba2`)

- `0a6920a` interrupt-pump fix (low-RAM kernel loops) — the original hard deadlock.
- `75f1468` Timer1/RCnt1 heartbeat telemetry.
- `e1dce63` IRQ raise/deliver/ack + bail-source ledger telemetry.
- `f1aaedd` `fntrace.c` dispatch-level bail-source capture (runtime-only).

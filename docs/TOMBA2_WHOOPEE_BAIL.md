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

### Mechanism (grounded model — binary truth + actual code + oracle-aware)

**Binary truth (PS-EXE header):** `0x80085900` / `0x8008AE50` are **NOT in the boot
EXE** (`t_addr=0x80010000`, `t_size=0x28800`, file = exactly that). They are a **disc
overlay loaded to high RAM (~0x80085000) at runtime**, executed by the **dirty-RAM
interpreter** (the boot EXE is small; the bulk of Tomba 2 is disc overlays).

So the freeze is at the **dirty-interp/overlay ⟷ compiled-BIOS boundary when an async
interrupt is delivered mid-call** — NOT a broken BIOS exception handler (that platform
infra works for Tomba 1 / MMX6 / Ape; per the recomp-debug "platform infra is almost
always correct" trap, doubting it was the mistake). My interrupt-pump fix (`0a6920a`)
is what now delivers interrupts *during* dirty execution, which exposes this.

**Root mechanism (confirmed in interrupts.c:355-384):** exception delivery sets
`COP0_EPC = 0x80000048` — a **host longjmp sentinel, NOT the real interrupted PC** —
and relies on saving/restoring GPRs around the handler `return;`. The recompiled
handler RFEs by setting `cpu->pc = EPC = 0x80000048`; `psx_unknown_dispatch`
(traps.c:524) longjmps on that sentinel. **Overloading the guest EPC as a host token
collides with the BIOS's *architectural* use of EPC** (it saves/restores EPC via the
TCB). When the interrupt is delivered mid-dirty-interp/overlay-call, the GPR
save/restore + sentinel-RFE does not preserve the interrupted continuation: the
resumed code's `$ra` is clobbered to `1` (degenerate steady state: `$ra=$sp=1`). The
next return is then "wild" (`$ra != stop_addr`), the contract bails+flattens,
re-dispatches a wrong target, and it **degenerates into an infinite bail loop** = the
68k/frame spin. (Interrupt is healthy at the controller level — raised/delivered/acked
1/frame — yet the guest *continuation* after RFE is corrupted.)

### Fix design (ChatGPT-converged, Architecture-A-legal)

1. **Do not overload guest EPC as a host longjmp token.** Keep the host
   exception-return mechanism as *separate runtime metadata*
   (`host_exception_return_kind` / `host_exception_jmp_target` /
   `exception_resume_pc`). At entry set `EPC = real_interrupted_pc`,
   `pc = exception_vector`, `in_exception=true` (let the BIOS do its TCB/EPC work
   normally). At RFE restore per RFE semantics; **do not invent or use `$ra`/`$sp` as
   host sentinels**; `restore_host_call_contract(frame)`.
2. The async interrupt path must either **fully suspend + restore the active host
   call-contract frame** across the exception, or **defer delivery to a committed
   guest safe point** (a valid resume point for an async HW interrupt — not HLE).

### Confirm-first (do this BEFORE the fix)

Single tripwire: latch the **exact moment `$ra` becomes `1`** (`old_ra != 1 &&
gpr[31] == 1`), capturing site (interp insn / exception-exit / bail), PC, prev `$ra`,
`in_exception`, frame, full regs — surfaced in the heartbeat. That pins whether the
clobber is an overlay instruction, the exception-restore, or the bail-flatten.

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

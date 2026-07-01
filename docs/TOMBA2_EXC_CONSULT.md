# Recomp consult — Tomba 2 exception-model fix (EPC de-overload)

Context for the "Recomp" GPT. This is a **static MIPS→C recompiler** (Architecture A):
the PSX BIOS (`SCPH1001.BIN`) is recompiled to native C; the game's disc overlays /
install-at-runtime RAM code are executed by a small **dirty-RAM interpreter**. There is
**no general interpreter and no HLE BIOS**. The exception/interrupt path is shared by
three already-shipped titles (Tomba 1, Mega Man X6, Ape Escape) — do not regress them.

## The bug (data-confirmed, not theory)

Tomba 2 boots and runs 60fps to frame ~1997, then the top-level dispatch returns
`cpu->pc=0` ("execution completed" abnormal exit). Measured root cause:

- Our async-interrupt delivery (`psx_check_interrupts`) sets **`COP0_EPC = 0x80000048`**,
  a host longjmp **sentinel** (a NOP we write), NOT the real interrupted PC. It then
  saves all guest GPRs to host locals, `setjmp`s, dispatches the recompiled BIOS
  exception handler, and on `ReturnFromException` **`longjmp`s back** and **restores the
  GPRs from the host locals**. So normal resume is a *host-stack return*, not an
  architectural RFE-to-EPC. The sentinel exists so the BIOS handler's COP2-branch-delay
  check reads a benign NOP.
- The recompiled BIOS handler saves EPC (= sentinel) into the **current TCB save area**.
  Confirmed live: TCB@0x108 → save_area off128 (EPC) = `0x80000048`, while off124 (ra) =
  `0x80050CC8` and off116 (sp) = `0x801FFFB8` are the correct interrupted context.
- Tomba 2 installs its **own** interrupt handler via **HookEntryInt (B0:0x19)**. That
  handler (`0x80085D8C`, dirty-interpreted) walks a per-source handler table and ends by
  calling **ReturnFromException (B0:0x17) itself** (full-takeover pattern).
- At frame ~1997 the game's handler calls `ReturnFromException` while **`in_exception=0`**
  (measured: `freeze_check` shows in_exception=0 + post_exception_cooldown active; the
  event ring's last event is a COOLDOWN gate, which is only set after in_exception
  clears). RFE reads `saved_epc = sentinel`, sets `cpu->pc = 0x80000048`; the
  sentinel→longjmp gate in `psx_unknown_dispatch` requires `in_exception`, so it FAILS;
  `0x80000048` is a bare NOP → resolves to `pc=0` → top dispatch exits.

**One line:** EPC is overloaded with a host sentinel; resume only works synchronously
inside our `psx_check_interrupts` window. The game drives `ReturnFromException`
*asynchronously* (outside that window), and the sentinel has no valid resume → exit.

## The key asymmetry that enables a fix

When the interrupt is delivered from the **dirty-RAM interpreter pump**
(`dirty_ram_dispatch`, every ~4096 insns), `cpu->pc` already holds the **precise next
guest PC** (the interpreter committed it at a clean poll boundary). Tomba 2's main code
is entirely dirty-interpreted, so a real interrupted guest PC **exists** at exception
entry — we are simply discarding it and writing the sentinel.

When the interrupt is delivered at a **compiled-code** block boundary, the interrupted
code resumes by *host return*, and there is no single precise guest PC — this is the
case the sentinel/host-restore hack was built for, and it works for T1/MMX6/Ape.

## Proposed fix (Option A — scoped EPC de-overload) — please pressure-test

When `psx_check_interrupts` enters an exception **and** the interrupted context is
dirty-interpreted (a valid guest resume PC is live in `cpu->pc`):
1. Set `EPC = cpu->pc` (real guest PC) instead of the sentinel. The TCB then saves the
   real PC.
2. `ReturnFromException` (sync or async) sets `cpu->pc = saved_epc = real PC`; the
   dispatch loop / dirty-interp re-entry resumes there. No host-stack longjmp needed for
   this path.
3. Keep the existing sentinel + host-GPR-restore + longjmp path **only** for
   compiled-interrupted contexts.

Open questions for you:
- For the dirty-interp path, do we still need the **host-GPR save/restore**? On real HW
  the BIOS handler saves/restores the full context via the TCB; if our recompiled
  handler does that faithfully, the host hack should be unnecessary on the real-EPC
  path — but the existing comment says the handler's *normal* (non-RFE) exit corrupts
  the interrupted GPRs. How do we cleanly separate "RFE exit" (resume via EPC) from
  "normal/longjmp exit" (resume via host) without the GPRs being clobbered?
- Is there a hazard where the **same** exception is entered from a *nested* dirty pump
  (we already had a dispatch-depth desync bug, fixed by save/restore of
  `g_psx_dispatch_depth`)? Does real-EPC resume interact with that?
- Async RFE with `in_exception=0`: should we drop the `in_exception` gate on the
  sentinel→longjmp entirely and instead **always** resume via `cpu->pc=EPC` when EPC is a
  real PC, reserving longjmp strictly for the sentinel-and-in_exception case?
- Alternative (Option B): force the game's HookEntryInt handler to run *inside* our
  exception window so the proven sync longjmp applies. We have not yet pinned WHY our
  model runs it with in_exception=0 (the BIOS DeliverEvent chain-walk that calls it
  appears to run post-cooldown). Is chasing the deferral safer than de-overloading EPC?

## Files
- `runtime/src/interrupts.c` — `psx_check_interrupts` (sentinel @ ~L362, save/restore,
  setjmp loop @ ~L443).
- `runtime/src/traps.c` — `psx_syscall` case 3 (ReturnFromException, ~L497),
  `psx_unknown_dispatch` sentinel gate (~L628).
- `runtime/src/dirty_ram_interp.c` — pump (`psx_check_interrupts` @ L1377), sentinel
  handling (L1388).

## Validation plan (the gate)
Oracle = Beetle PSX (`psx-beetle.exe`, port 4380) on the same BIOS+disc. After the fix:
Tomba 2 must roll past the splash into the intro FMV; **and** Tomba 1 / MMX6 / Ape must
remain frame-for-frame healthy vs the oracle.

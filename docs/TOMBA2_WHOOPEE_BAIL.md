# Tomba 2 — Whoopee-Camp splash freeze (wild-call/return-contract bail storm)

Branch: `wt/tomba2`. Single source of truth for the splash freeze. Update as we learn.

---

## 🎯 STATUS 2026-06-22 sess2 — FRAME-1997 EXIT FIXED; new frontier = splash→attract

**FIXED (commit d016e3b):** the frame-1997 abnormal exit. Tomba 2 now runs past it —
the Whoopee-Camp splash renders and the frame climbs steadily (16k+), `dirty_ram_aborts=0`.
Root cause was **longjmp-landing-depth** (NOT async, NOT EPC de-overload): a game RFE
longjmp at a BLOCK-RETURN pump left `cpu->pc=0`, which unwinds to the outermost dispatch
and reads as a clean exit. Fix = restore the committed guest PC after the block-return
pumps so the trampoline re-dispatches (details in the CORRECTION section below).

**NEW FRONTIER — splash→attract divergence (first-divergence, NOT a CD bug).** The recomp
softlocks on the splash; the Beetle oracle on the same disc reaches the **attract/intro
gameplay** (3D Tomba on a grassy field). BOTH sit at the same idle PC `0x80050CE8`, so the
recomp tracks the oracle's CODE but has diverged in game STATE.

Investigated 2026-06-22 sess2:
- The splash main loop is at `0x80050C40+`; it pumps a 2-frame VBlank gate (counter
  `0x800E809C` reset by `sh zero` @`0x80050C8C`, waited up to threshold byte `[0x1F800235]=2`
  — works, reaches 2 between samples) and runs a state machine on `[0x1F8001A4]` (observed
  stuck at 0). The state never advances to load/show the attract.
- **CD RULED OUT.** The attract data DOES load correctly: consecutive CD-DMA dest addresses
  (`0x80041800`, `0x80042000`, …) hold DIFFERENT, valid MIPS code, and `read_msf` advanced to
  LBA 374. The `cd_read_log` "lba" field misleadingly logs the SetLoc target (24) for every
  DMA read, not the per-sector read position — a TOOL bug to fix (Rule 15): it cost a wrong
  "CD stuck at LBA 24" hypothesis. The CD streaming/advance is fine.
- So the divergence is a true first-divergence in the splash state machine / scene-transition
  decision, upstream of the common `0x80050CE8` loop. Next: oracle PC-trace from the splash
  (psx-beetle @4380) to find the first divergent branch / state write that gates the attract.

**SECONDARY: ~frame-17k crash.** With the frame-1997 fix the recomp runs ~16-17k frames on
the splash (≈8e9 dirty insns; ~470k insns/frame — the splash spin is expensive) then dies
silently (SEH; watchdog was off). Possible host-stack growth from the fix's re-dispatch on
NESTED block-return-pump RFEs (the restore fires ~1/22 frames) — must check (also a
regression-risk signal for the shared path). Re-run with watchdog + crash dumps armed.

**PENDING GATES (before master-landing of the fix):** (1) T1/MMX6/Ape regression — the
fix is in the SHARED dirty-pump path; argued low-risk (the restore only fires when a
handler RFE clears a valid committed PC, which playable titles don't hit) but NOT yet
run. (2) Clean up the dead async-EPC scaffolding (inert under the corrected model).

---

## ✅ RESOLVED (2026-06-22) — root cause was the overlay-region FLOOR, not a contract bug

The splash freeze is **fixed**. Both prior theories below (exception/EPC, then "B0
non-local-return contract bail") were **symptoms, not the cause** — they were
artifacts of the real bug. Data-confirmed root cause and fixes:

**Root cause: `OVERLAY_REGION_FLOOR` was hardcoded to Tomba 1's main-EXE text end
(`0x98000` = `0x10000 + 0x88000`).** Tomba 2's boot EXE is only `0x28800`, so its
text ends at **`0x38800`** and its disc overlays load at **`0x80085000`+** — i.e.
in the range `[0x38800, 0x98000)` that the hardcoded floor wrongly classified as
"statically-recompiled main-EXE text." Consequences for every Tomba 2 overlay:
1. `allow_local_dirty_flow` / `is_local_dirty_target` were **false**, so overlay
   code ran **block-by-block** (each guest branch surfaces to the trampoline and
   re-dispatches). A ~295 KB overlay-init `memset` at `0x800896F0` became ~73 K
   trampoline round-trips (the "slow" red-herring symptom).
2. Intra-overlay calls routed through **`dispatch_nonlocal_call` → `psx_dispatch_call`
   with a live `stop_addr` contract** instead of chaining in-interpreter. When the
   per-frame interrupt pump fired mid-contract, the contract was violated and
   **bail/flatten cascaded into the `$ra=$sp=1` storm** (the symptom the prior
   handoff chased as a "B0 wild-return contract bug").

**Evidence:** the §18 xprobe ring + dispatch_tail + live disasm showed the trigger
code (`0x80085900`, `0x800896F0`, `0x8008AE50`) all at phys `0x85xxx`–`0x8Axxx`,
**below** the `0x98000` floor; the header comment itself (`dirty_ram_interp.h:55`)
documented the floor as "Tomba: load 0x10000 + text 0x88000".

**Fix 1 (overlay floor, the root fix):** `OVERLAY_REGION_FLOOR` is now the runtime
variable `g_overlay_region_floor`, pinned at game load to
`(gc.load_address + gc.text_size) & 0x1FFFFFFF` (= `0x38800` for Tomba 2).
Default `0x98000` for BIOS-only. For Tomba 1 this evaluates to the same `0x98000`
(no behavior change); it is correct-by-construction for every game. Files:
`dirty_ram_interp.h`, `dirty_ram_interp.c`, `main.cpp`. **Result: bail storm gone
(`bail_first=0`), splash advances at 60 fps past the old frame-1823 wedge.**

**Fix 2 (latent dispatch-depth desync, exposed next in the cascade):** with overlays
now on the local-flow path, the per-4096-insn pump delivers interrupts from a
**nested** dispatch (depth > 0). `psx_check_interrupts` reset `g_psx_dispatch_depth`
to 0 around the handler but never restored the interrupted code's nesting, and a
ReturnFromException longjmp skips the handler frames' decrements — so the outer
frames unwound to **`dispatch_depth = -1`**, the `outermost` test misfired, and the
top-level dispatch returned to PC=0 ("execution completed" exit). Fix: save the
depth before the handler, run the handler at 0, restore after. File: `interrupts.c`.

**Current cascade frontier (NEXT bug) — PINNED 2026-06-22:** with both fixes Tomba 2
runs cleanly to **frame ~1997**, then the **top-level dispatch returns `cpu->pc=0`**
("execution completed" abnormal exit; `dispatch_depth=0`, so NOT the depth bug).

Pinned via `PSX_FNTRACE_ARM=all` (exit-trace dispatch tail) + `PSX_EXIT_HALT=1`
(halt-and-serve at the PC=0 exit, overlays still loaded) + live disasm:
- `last_store_pc = 0x80086208` decodes to `li $t2,0xB0; jr $t2; li $t1,0x17` =
  a call to **B0:0x17 `ReturnFromException`**. The adjacent overlay thunks are
  `0x80086210` → **B0:0x18 ResetEntryInt** and `0x80086220` → **B0:0x19 HookEntryInt**.
- So **Tomba 2 installs its OWN interrupt handler via `HookEntryInt` and returns
  from it by calling `ReturnFromException` directly.** Its per-frame VBlank callback
  (`0x80085D1C → … → 0x80086200`) drives this.
- Our exception model assumes the *default* BIOS path: `psx_check_interrupts` sets
  `COP0_EPC = 0x80000048` (a host longjmp sentinel, NOT the real interrupted PC),
  dispatches the handler, and resumes via `setjmp/longjmp` + a host GPR save/restore
  — it does **not** drive the BIOS's real EPC/TCB exception save+restore that
  `ReturnFromException` depends on. When the game's hooked handler calls
  `ReturnFromException`, the RFE-to-sentinel eventually resolves to `cpu->pc=0`
  (in_exception already cleared at the exit) → top dispatch returns → exit.

This is the **EPC-overload tension** the (refuted-for-the-bail-storm) design notes
below flagged — refuted as the bail cause (that was the floor), but it **is** the
mechanism here. **Fix direction (delicate, risk of regressing T1/MMX6/Ape):** make
the exception model support a game-installed `HookEntryInt` handler that calls
`ReturnFromException` — i.e. stop overloading guest EPC as a host token; keep host
exception-return state as separate runtime metadata; set `EPC = real interrupted
PC`, let the BIOS/kernel exception path (and the game's hooked handler) run, and
restore the host call-contract frame on RFE. Validate against the oracle
(psxref @4380) frame-by-frame. Diagnostic tooling in place: `PSX_EXIT_HALT`.

---

## ✅✅ CONFIRMED with live data (2026-06-22, session 2) — was inference, now proof

Reproduced the frame-1997 exit (`PSX_EXIT_HALT=1`, halt-and-serve) and read the real
state over TCP 4500. The prior section was an *inference from `last_store_pc`*; the
following is *measured*:

**1. The exit sequence (from the `PSX_FNTRACE_ARM=all` dispatch-tail ring):**
```
0x80085D1C  game VBlank entry (sp -> 0x800ABD30, game stack)
0x000000B0  -> B0 vector, ra=0x80085F3C   = ReturnFromException (B0:0x17)
0x80000048  RFE restored interrupted ctx (sp=0x801FFFB8 ra=0x80050CC8), pc=SENTINEL
            -> psx_unknown_dispatch sentinel-longjmp gate FAILS -> pc=0 -> top exit
```

**2. `in_exception == 0` at the failing RFE — measured two independent ways:**
- `freeze_check`: `in_exception=0`, `post_exception_cooldown=7`, `current_func=0x000000B0`,
  `last_store_pc=0x80086208`.
- The event ring's last entry is `IRQ_GATE detail=2` = `GATE_COOLDOWN`, which is only
  set *after* `in_exception` is cleared. So the game's RFE ran in the post-exception
  window, not inside our synthesized exception.

**3. The TCB save area is the smoking gun.** `TCB ptr @0x108 = 0xA000E1EC` →
save_area `0xA000E1FC`:
```
saved EPC (off 128) = 0x80000048   <-- OUR SENTINEL (the bug)
saved SR  (off 140) = 0x40000404   (exception-pushed SR)
saved ra  (off 124) = 0x80050CC8   (real interrupted ctx, correct)
saved sp  (off 116) = 0x801FFFB8   (real interrupted ctx, correct)
```
The interrupted GPRs are saved correctly; only **EPC is the sentinel** instead of the
real interrupted PC. `interrupts.c:362` writes the sentinel; the recompiled BIOS
handler saves it to the TCB; `ReturnFromException` reads it back and RFEs to the
sentinel.

**4. The game handler is HookEntryInt-installed and self-RFEs (disasm of live RAM):**
`0x80085D8C` walks a per-source handler table (`jalr ra,v0` loop `0x80085E2C–E78`) and
ends with `jal 0x80086200`. `0x80086200 = addiu t2,0xB0; jr t2; addiu t1,0x17` =
B0:0x17 ReturnFromException; `0x80086220` = B0:0x19 HookEntryInt (the install site is
`0x80085D3C jal 0x80086220`). So Tomba 2 installs a full-takeover interrupt handler
that drives `ReturnFromException` itself.

**5. A precise guest resume PC EXISTS and is being discarded.** The interrupted code is
dirty-interpreted (`event_ring` mode=INTERP, pc≈`0x80050CE8`). The dirty-interp pump
(`dirty_ram_interp.c:1377`) commits `cpu->pc` (the next guest PC) **before** calling
`psx_check_interrupts`. So at exception entry `cpu->pc` holds the real interrupted
guest PC — but `psx_check_interrupts` ignores it and writes the sentinel into EPC.

### Root cause (one line)
Our exception model overloads COP0 EPC with the host sentinel `0x80000048`. Resume
works only via the host-GPR-save/restore + `longjmp` hack, which is valid **only
synchronously inside `psx_check_interrupts`**. Tomba 2's HookEntryInt handler calls
`ReturnFromException` **asynchronously** (after our exception window has closed,
`in_exception=0`); RFE reads `saved_epc=sentinel`, the longjmp gate fails, and the
sentinel resolves to `pc=0` → "execution completed" abnormal exit.

### Fix design (recommended — pending ChatGPT consult + oracle grounding)
**Option A (scoped EPC de-overload — recommended):** when `psx_check_interrupts`
delivers an interrupt while the interrupted context is dirty-interpreted (a valid
guest resume PC is live in `cpu->pc`), set `EPC = cpu->pc` (real guest PC) instead of
the sentinel. The TCB then saves the real PC; `ReturnFromException` — synchronous OR
asynchronous — resumes at the real guest PC via the dispatch loop / dirty-interp
re-entry. Keep the sentinel + host-restore path ONLY for compiled-interrupted
contexts (where no precise guest PC exists; T1/MMX6/Ape gameplay handlers RFE
synchronously and are unaffected). The host-GPR-save/restore interplay on the
real-EPC path must be re-derived carefully.
**Option B:** pin *why* our model runs the game handler with `in_exception=0` and make
it run inside the exception (reuse the proven sync longjmp). Needs the deferral
mechanism nailed first.
**Gate:** this touches the shared interrupt path — re-regress Tomba 1 / MMX6 / Ape
frame-by-frame vs the oracle before landing on master.

Consult package for the Recomp GPT: `docs/TOMBA2_EXC_CONSULT.md`.

---

## ⚠️ CORRECTION (2026-06-22 session 2, impl pass) — it is NOT an async (in_exception=0) RFE

Implementing the "Option A async-resume" idea and instrumenting it **refuted the
in_exception=0 framing above**. Measured (freeze_check counters added this pass):
`reach_dirty = 19921` (≈ one per exception), **`dirty_in_exc0 = 0`**, `async_rfe_fire = 0`.

So the sentinel `0x80000048` is dispatched once per exception and **ALWAYS arrives at
the `dirty_ram_dispatch_inner` gate with `in_exception == 1`** — it always takes the
`psx_exception_longjmp()` branch. There is **no** `in_exception==0` dispatch of the
sentinel. (The earlier "in_exception=0 at the RFE" reading was the POST-exit state:
the botched longjmp landing clears `in_exception` and sets the cooldown gate.)

**Corrected mechanism (longjmp-landing-depth, not async):**
- The game's HookEntryInt handler calls `ReturnFromException`; the recompiled BIOS RFE
  restores the interrupted GPRs from the TCB and sets `cpu->pc = saved_epc = sentinel`.
- The trampoline dispatches the sentinel → `dirty_ram_dispatch_inner` sets `cpu->pc = 0`
  then `psx_exception_longjmp()` (in_exception==1).
- The longjmp lands in `psx_check_interrupts`' `setjmp` loop (jmp_val==1), which restores
  GPRs and **returns** — but `cpu->pc` is still 0. Control returns up through the pump
  (`dirty_ram_dispatch` → trampoline).
- For ~19920 exceptions the longjmp lands at a **nested** dispatch (depth>0) and the
  enclosing dirty-interp flow re-commits `cpu->pc`, so the game continues.
- For the failing one the longjmp unwinds to the **outermost** trampoline (depth 0),
  where `cpu->pc == 0` → `psx_dispatch` returns → main.cpp "execution completed" exit.

So the bug is: **a `ReturnFromException` longjmp can unwind to the OUTERMOST dispatch and
leave `cpu->pc = 0`, which the top loop reads as a clean exit.** The `g_psx_dispatch_depth`
fix (d10bc12) addressed depth desync but not this: the longjmp itself reaches `psx_dispatch`'s
outermost frame with a null PC. The single global `exception_jmpbuf` + the "resume via host
return" model is the fragile core (matches the consult's "host continuation state bleeding").

**Candidate fix (NOT yet implemented/validated):** when the RFE longjmp lands in
`psx_check_interrupts` (jmp_val==1) and `cpu->pc == 0`, set `cpu->pc =
g_async_rfe_resume_pc` (the latched real interrupted guest PC) before returning, so the
trampoline re-dispatches the guest instead of treating the null PC as an exit. MUST be
checked against the ~19920 normal sync RFEs (how do THEY restore the resume PC today? —
trace one normal landing vs the exit landing) and re-regressed on T1/MMX6/Ape.

**Tooling added this pass (kept, runtime-only):** `freeze_check` now reports
`async_rfe_set/fire`, `reach_dirty/traps`, `reach_async`, `dirty_safe_resume_pc`,
`async_rfe_resume_pc`; `g_dirty_safe_resume_pc` latched around the 3 dirty pumps;
`g_async_rfe_resume_pc` latched at dirty-safe exception entry. The async-resume GATE code
(traps.c / dirty_ram_interp.c sentinel branches) is present but **never fires** under the
corrected model — it is scaffolding for the candidate fix, not a working fix.

---

### Original investigation (history — superseded by the RESOLVED section above)

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

### CORRECTION (confirm tripwire, commit d965d6e) — it is NOT the exception path

The `$ra->1` tripwire + first-bail `in_exception` latch **refuted the exception/EPC
model above** (data over theory, per recomp-debug):

- `bail_first_in_exc = 0` — the `0xB0 -> 0x8008AE50` trigger bail is **NOT** during an
  exception.
- `ra_tw`: site = INTERP, `in_exc = 0`, `pc = 0x8008B018`, `prev_ra = 0x8008AF10` -> 1.
- `0x8008B018` disassembles to **`lw $ra, 0x34($sp)`** — a normal function **epilogue**
  restoring `$ra` from the stack. The loaded value is `1`, so the **saved `$ra` on the
  stack is corrupt** (the stack/`$sp` is wrong), and this happens in **plain
  non-exception execution**.

**Corrected mechanism:** a B0 syscall (reached inside `jal 0x80085900` from caller F at
`0x8008AE48`) returns **non-locally** to F's continuation `0x8008AE50` (a tail-call
shape) — *not* an interrupt. Our bail/flatten handles the control transfer but leaves
the **stack / `$sp` wrong** (the skipped frame isn't unwound / `$sp` not restored).
Later F's epilogue `lw $ra, 0x34($sp)` reads a corrupt saved-`$ra` (`1`) → degenerate
`$ra=$sp=1` → infinite bail loop. So the real bug is a **control-flow / stack-contract
bug in the bail/flatten handling of a B0 non-local (tail-call) return** (recomp-debug
Class 3), NOT exception/RFE/EPC. The EPC-separation fix below is therefore the WRONG
fix and is retained only as a refuted hypothesis.

### Revised fix direction (data-driven)

The bail/flatten of a B0 (kernel-vector) non-local return from dirty/overlay code must
restore the **correct guest `$sp`** (and unwind the skipped frame) so the resumed
caller's stack is valid — OR the B0-vector tail-call must be dispatched as a tail
transfer (no spurious `stop_addr`) rather than a call whose contract bails. Next probe:
trace `$sp` and the `stop_addr` across the first B0 bail (latch them at the first bail),
and read what `0x80085900` does at its end (tail `j`/`jr 0xB0` vs `jal`).

### (REFUTED) Fix design (ChatGPT-converged, Architecture-A-legal)

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

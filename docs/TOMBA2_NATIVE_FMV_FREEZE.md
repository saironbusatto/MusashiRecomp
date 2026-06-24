# Tomba 2 — native-overlay FMV freeze (end of Whoopee Camp): root-cause findings

Status: **ROOT CAUSE LOCALIZED** (2026-06-23). Deterministic codegen divergence
in a native-compiled overlay function. NOT a timing/race. Exact diverging
instruction inside the suspect call tree not yet pinned; recompiler class fix
pending.

## Symptom

With the warm overlay cache delivering full native execution (emu_cpu ~16.5ms /
60fps, dirty-interp idle, `dispatch_interp_fallback=0`, `reval_crc_miss=0`,
`invalidations=0`), the boot **hard-freezes at guest frame ~1823** at the end of
the Whoopee Camp FMV. `overlay_native_off` (route overlays through the sanctioned
dirty-RAM interpreter instead of native DLLs) makes it sail past. Near-deterministic
freeze frame (1823 ×3, 1831 ×1).

This is the same ~frame-1828 hang a prior session believed fixed by the
alias/interior-entry rollup (commit 47f3f15). It was not fixed — it was MASKED:
the messy accumulated captures audit-failed the relevant region's compile, so
those overlay functions fell back to dirty-interp (correct). A clean warm cache
compiles the region successfully → it runs native → the divergence re-surfaces.

## Freeze state (always-on rings)

- `irq_state`: `i_stat=0x01` (VBLANK pending), `i_mask=0x0C` (VBLANK bit0 MASKED).
  `cop0_sr=0x40000401` (IEc=1, IM2=1). The pending VBLANK can never be taken.
- `dirty_ram_stats`: `insns_run=0` (interp idle). `overlay dispatch_native` climbing,
  `dispatch_interp_fallback=0` → the guest is busy-spinning in NATIVE code.
- `dispatch_tail`: last 64 dispatch targets all `0x000000A0` (kernel A0 syscall
  vector). `dispatch_stats static_hits=25M, miss=0`. The mainline re-enters the A0
  vector in a tight loop — an event-wait that never completes.
- `imask_trace`: under native, every i_mask write is BIOS ROM `0xBFC12210`; the
  game's own critical-section handler `0x80085CB0` (base mask 0x0D, VBLANK on)
  NEVER runs. Under dirty-interp it does, and the game progresses.

## Proof it is CODEGEN, not a race

The runtime already has a **sandboxed native↔interp differential** (built a prior
session — see `SHADOW_ENHANCEMENTS.md`):

- `overlay_diff_on` / `overlay_diff_off` — each matched overlay function runs BOTH
  ways from identical CPU+RAM state; the interp result is kept (the game stays
  correct and PROGRESSES), and native computation divergences are logged.
- `overlay_shadow_dump` / `overlay_shadow_detail` — the divergence records.
- `overlay_native_ring` — always-on ring of native overlay calls + in-progress entry.
- `overlay_irq_ratelimit` / `overlay_irq_suppress_*` — independently test whether the
  divergence is interrupt-delivery timing.

Method: `overlay_diff_on` early (frame 793), ran PAST the freeze (frame 2344) on
interp-kept results, `overlay_shadow_dump`.

Because the shadow runs both engines from IDENTICAL state with no interrupts and no
timing, a divergence there is purely computational. **Native diverges → deterministic
codegen bug, definitively not a race.**

## First divergence

`overlay_shadow_dump` seq 1 (earliest), and `overlay_shadow_detail`:

```
function 0x800896E0   (a stack/heap/context init; see disasm below)
  t1 (r9):  native 0x000000E4   interp 0x00000039
  t0 (r8):  native 0xBFC01E24   interp 0x80000000
```

- `0xE4 = 0x39 << 2` → native executed the **A0 dispatcher's `sll t1,t1,2`**
  (kernel A0 dispatch stub at 0x5C4: `addiu t0,zero,0x200; sll t1,t1,2; add; lw t0,0(t0); jr t0`).
- `0xBFC01E24 = A0_table[0x39]` (= `*(0x200 + 0x39*4)` = InitHeap's ROM entry).
- interp `t0=0x80000000` comes from `0x80089714: lui t0,0x8000` in 0x800896E0 itself;
  interp `t1=0x39` is the untouched entry value.

Interpretation: **native erroneously routes through the A0:0x39 (InitHeap) dispatch
path that the interpreter does NOT take.** It re-inits every spin-loop iteration and
never satisfies the loop's exit condition → endless A0-vector dispatch → VBLANK stays
masked (the game disabled it via a B0 syscall, `imask 0x0D->0x0C`, and the path that
would re-enable it never runs) → deadlock.

Spin set from `overlay_native_ring`: `0x80050B08 → 0x80089788 → 0x800896E0`
(in-progress), cycling `0x89770` / `0x89860`. The actual wrong branch is somewhere
in the 0x800896E0 callee tree (it decides to take the A0:0x39 path); shadow currently
gives function granularity, not the exact instruction.

### 0x800896E0 disasm (head)

```
800896e0: lui v0,0x800c ; addiu v0,v0,-7976      ; v0 = 0x800CE0D8
800896e8: lui v1,0x8010 ; addiu v1,v1,25128      ; v1 = 0x80106228
800896f0: sw zero,0(v0) ; addiu v0,v0,4 ; sltu at,v0,v1 ; bne at,..0x896f0  ; memset 0x800CE0D8..0x80106228
80089704: lui v0,0x800a ; lw v0,16264(v0)         ; v0 = *(0x800A3F88)
80089710: addi v0,v0,-8
80089714: lui t0,0x8000 ; or sp,v0,t0             ; sp = (v0-8) | 0x80000000   <-- interp t0 here
8008971c: ... builds a0 from 0x80106228, a1 = size, stores to 0x800Bbef8/bef4
80089758: sw ra,-7976(at) ; addiu gp,..; addu fp,sp,zero
80089768: jal 0x80089860 ; addi a0,a0,4
```

## Suspected codegen construct (to confirm)

ChatGPT consult (Overlay Cache Architecture chat, with our debug principles
shared) ranked, for this static-MIPS CPS recompiler:
1. **delay-slot / continuation-PC handling at a cross-unit control transfer around
   an interior/alias entry** (the area commit 47f3f15 touched) — top suspect.
2. R3000A load-delay feeding a branch/jump/syscall-index (see separate finding below).
3. entry-switch fallthrough to the wrong interior block.

## Separate latent CLASS bug: recompiler does not model R3000A load-delay

- `recompiler/src/strict_translator.cpp:830` — "No load-delay-slot modeling: per
  project decision, the recompiler writes the destination register at the load
  instruction's position ... relies on the BIOS already respecting the architectural
  load-delay rule (assemblers/compilers schedule loads correctly)."
- The dirty-RAM interpreter (`runtime/src/psx_interpreter.c`) DOES model it faithfully
  (`set_load_delay`/`apply_load_delay`, 1-instruction pending-load slot).
- That assumption is VIOLATED in this game's overlays: a scan found **50 load-delay
  hazards** (load rt immediately followed by an instruction reading rt) in the resident
  overlay code — **0 of them control-flow**, and the immediate 0x896E0 freeze path had
  none. So load-delay is a real latent divergence class (worth a class fix), but is NOT
  proven to be THIS freeze.
- Class fix (future): detect load-delay hazards in the recompiler and emit the
  old-value-preserving (deferred-write) semantics for the hazard pair, matching the
  interpreter. No load-delay hazard detection currently exists (only a branch-delay
  scanner, `tools/scan_branch_delay_hazards.py`).

## Deeper investigation (2026-06-23, session 2) — codegen DOUBLY confirmed; red herrings ruled out

Raw RAM dumped + loaded into Ghidra (MIPS:LE:32 @ 0x80000000). Dumps are now
KEYED by resident-overlay identity (tools dump_ram.py writes
`<name>_f<frame>_<variant-fingerprint>.bin` + a .json manifest of game/frame/
resident-fn CRCs) — a bare RAM image is ambiguous because the same addresses
hold different overlay variants per scene.

Codegen-vs-race settled by TWO independent methods:
1. Sandboxed `overlay_diff` shadow (identical state, no IRQ/timing) diverges.
2. `overlay_irq_ratelimit n=4096` (coarse, interp-like IRQ cadence) set BEFORE
   the freeze → STILL freezes at ~1826. Not IRQ-cadence/timing.
=> CODEGEN, definitively not a race.

Red herrings ruled OUT as the direct cause:
- BREAK no-op: the spin-set's break is at 0x80089784. The dirty-RAM interpreter
  (`dirty_ram_interp.c:1041`, the engine under overlay_native_off) routes break
  to `psx_break` which CRASHES (traps.c:594 = trap_crash + exit(1)). Since
  overlay_native_off PROGRESSES (no crash), 0x89784 is NEVER reached in the
  working path => main (0x80050B08) does not return there normally. So the
  recompiler's break-no-op (code_generator.cpp:973) is a real latent bug (should
  fail-loud like strict_translator's `psx_break(...)`/the interp), but it is NOT
  what breaks the FMV. It only matters once something upstream wrongly reaches it.
- Load-delay: path clean (see above).

Operative divergence (what actually happens): under native, control flow reaches
the bootstrap re-loop — the native overlay ring shows a repeating cycle
{0x89860 InitHeap-thunk → 0x89770 → 0x80050B08 "main" → 0x89788 init-array} with
0x800896E0 (the crt0-style stack/heap/context init) re-entered each lap, calling
InitHeap (→ A0 vector) every iteration — which is the `dispatch_tail` all-0xA0
spin. Under interp this does not loop. i.e. native's 0x80050B08 ("main") RETURNS
(native_ring returned:1) or the bootstrap otherwise re-enters, where interp keeps
running. Root is a mis-emitted control transfer in the 0x80050B08 subtree (large
fn, internal loops, many interior/alias entries) — NOT yet pinned to one insn.

Shadow caveat (rule 15): `overlay_shadow_dump` reports 13 divergences; the seq-1
(0x896E0 t0/t1) is suspect as a SYSCALL-BOUNDARY ARTIFACT — native continues
through the A0 dispatch (t1=0x39<<2=0xE4, t0=A0_table[0x39]) while the interp
shadow stops at the overlay→kernel(0xA0) boundary (t1=0x39). Later records have
real RAM divergences (e.g. fn 0x85D70: RAM native=0 vs interp=0xBFC050A4, a BIOS
ROM pointer) suggesting a systemic BIOS-interaction/return-path codegen issue,
not one function. Need finer (instruction-level) isolation.

## Session 3 (2026-06-23): per-function native-disable BUILT + bug localized to func_80050B08

Built the per-function native-disable (the tool ChatGPT recommended): a phys-keyed
blocklist in overlay_loader.c that forces a named overlay function through the
sanctioned dirty-RAM interpreter (NOT skip/stub/HLE — it still runs), exposed as the
`overlay_native_block` debug command ({"addr":"0x.."} add / {"clear":1} clear /
{} report). Runtime-only change (overlay_loader.c + debug_server.c), no regen.

BISECTION RESULT — culprit is **func_80050B08's own native body**:
- `overlay_native_block addr=0x80050B08` set early → boot PASSES the freeze (frame
  2306+), hits=1. Because the blocked function runs via interp while its CALLEES stay
  native, this proves the bug is in 0x50B08's OWN compiled code, not a callee.
- 0x80050B08 = the game main: a 33-`jal` subsystem-init sequence then an infinite main
  loop (no `jr $ra`; loops via goto). It is entered ONCE (hits=1); native exec of it
  causes the bootstrap re-loop / A0-spin; interp exec runs the game (frame advances).
- Verified clean: all 33 CPS jal TARGETS match the RAM-decoded jal instructions; all
  jal return-continuations are present in the entry-switch (no missing/`default`
  fall-through-to-top). So it is NOT a wrong-target or missing-continuation bug.

NARROWED to the main-loop region (block_80050C6C..0x50DE0, the part with real
conditionals). STRONG SUSPECT: an INTERIOR ENTRY in a loop — `block_80050CE8` is an
entry-switch case landing MID-BLOCK, immediately AFTER `0x50CE4: lhu v0,-32612(a0)`,
so entering at 0x50CE8 uses a STALE v0 (skips the load) before the `sltu v0,v0,v1` +
`bne` scan-loop test. This matches ChatGPT's #1 suspect (continuation/interior-entry
handling, the 47f3f15 zone). Need to confirm what transfers control to 0x50CE8 (a
jalr-return / jr-table / branch alias) and whether native lands there with wrong
state vs interp.

## Next steps

1. Build a PER-FUNCTION / per-entry-PC native-disable (ChatGPT's #1 tool; does not
   exist yet — only the global overlay_native_off). It must route the disabled
   HostUnit through the sanctioned dirty-RAM interpreter (NOT skip/stub/HLE). Then
   bisect: start by disabling native for 0x80050B08, then its callees, to find the
   single function whose native execution flips the freeze. This gives causal proof
   where the shadow (function-granular, with syscall-boundary artifacts) cannot.
2. With the culprit function isolated, instruction-diff native-vs-interp within it
   (extend the shadow to record the first diverging guest PC, or step the dirty
   interp and the native side from the same entry and compare per-block) to pin the
   exact mis-emitted transfer.
3. Confer with ChatGPT on that disasm to confirm the codegen construct (likely a
   jr/jalr/branch continuation-PC or interior/alias-entry transfer in 0x50B08's tree).
4. Class-fix in `recompiler/src` (never generated C, never a Tomba 2 workaround).
5. Verify: regen overlay cache, `overlay_diff` shows 0 divergences, live native run
   passes the freeze (frame > 2031) at 60fps with MDEC advancing.
6. Adjudicate the exact divergent MIPS against the Beetle oracle + BIOS disasm + Ghidra;
   generated C is evidence only.

Separately (independent class fixes, lower priority): emit BREAK as fail-loud/exception
in code_generator.cpp (match strict_translator/interp), and model the R3000A
load-delay slot.

Diagnostic principle reminder: a shadow mismatch is "native differs from interp at
fn X / insn Y", NOT automatically "native is wrong" — the interpreter can also be
wrong; adjudicate against Beetle/disasm before committing a fix.

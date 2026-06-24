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

## Next steps

1. Drill to the exact diverging instruction inside the 0x800896E0 callee tree (it makes
   an A0:0x39 call the interpreter doesn't — find the branch whose condition native
   computes wrong). Disassemble 0x80089860 and the A0-call site; narrow with the diff
   infra.
2. Confer with ChatGPT on the disasm to confirm the codegen construct.
3. Class-fix in `recompiler/src` (never generated C, never a Tomba 2 workaround).
4. Verify: regen overlay cache, `overlay_diff` shows 0 divergences for 0x896E0, and a
   live native run passes the freeze (frame > 2031) at 60fps with MDEC advancing.
5. Adjudicate the exact divergent MIPS against the Beetle oracle + BIOS disasm + Ghidra;
   generated C is evidence only.

Diagnostic principle reminder: a shadow mismatch is "native differs from interp at
fn X / insn Y", NOT automatically "native is wrong" — the interpreter can also be
wrong; adjudicate against Beetle/disasm before committing a fix.

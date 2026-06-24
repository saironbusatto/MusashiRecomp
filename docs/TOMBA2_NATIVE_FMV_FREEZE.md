# Tomba 2 — native-overlay FMV freeze (end of Whoopee Camp): root-cause findings

Status: **FIXED + VERIFIED** (2026-06-23, wt/tomba2 bd60381). Was a deterministic
overlay range-ownership / dispatch-legality bug (not a race). See "RESOLUTION" at
the very bottom for the implemented fix and verification; the sections below are the
investigation trail that led there.

---

## RESOLUTION (implemented + verified)

Fix shipped: the fail-closed native entry guard (the runtime/dispatch-legality layer
of the ChatGPT-validated plan; "ownership is not entry legality"). The generated CPS
entry-switch for OVERLAY functions no longer falls through to the function top on an
unknown interior PC — it restores the PC and calls psx_native_bad_entry, and the
overlay dispatch routes that PC to the sanctioned dirty-RAM interpreter (the live
bytes run correctly; NOT a stub/HLE). A true-prologue entry still runs from the top.
Overlay-scoped via config_.overlay_mode (static BIOS/boot-EXE keep legacy behavior;
their generated trampoline doesn't consume the signal and their ranges have no
multi-variant ambiguity). Wired through OverlayCallbacks (ABI v5→v6 to reject stale
caches), the DLL glue (compile_overlays.py), and the runtime (overlay_loader.c,
cpu_state.h). Files: recompiler/src/code_generator.cpp, runtime/src/overlay_loader.c,
runtime/include/{cpu_state.h,overlay_api.h}, tools/compile_overlays.py.

VERIFIED (clean-warm native): boot passes the freeze — frame 1823 → 18001+ at
emu_cpu 16.5ms / 60fps, i_mask=0x0D (VBLANK live), reval_crc_miss=0, invalidations=0,
MDEC decoding; foreign-interior PCs (0x89788) fail closed to the interpreter
(dispatch_interp_fallback; cheap flag-guarded init-array).

DONE — B/C completeness (the discovery/ownership layer; recompiler main_psx.cpp overlay
mode): in overlay discovery, after walking the seed roots, every DIRECT CALL (jal)
target that lands strictly INSIDE an already-discovered function is registered as an
ALIAS ENTRY of that host (the existing alias machinery — materialize_alias_groups), so
the host's entry-switch routes a dispatch of that PC to the correct absorbed interior
block (runs NATIVE), instead of relying on it having been captured/executed. This is
derived purely from static jal evidence, so it does not depend on the capture. Why this
over an explicit range-clip: the absorbed code IS the right code; it just needed a
routable entry. VERIFIED: regenerated overlay cache (codegen hash cg4_42de29a8)
func_800896E0 now emits `case 0x80089788u`; the cps_probe for 0x89788 drops from 1021
hits (broken CPS find-by-range -> func_800896E0 default) to 0 (now a registered entry,
main-chain native); freeze still gone, 60fps sustained to frame 6261+. The fail-closed
default remains as the safety net for any genuinely-unrouted interior PC.
NOTE: the static two-phase FunctionDiscovery::discover already enforces hard_cap = next
known entry (in_bounds: addr < hard_cap), so only the overlay path needed this.

STILL OPEN: NOT yet visually confirmed split-frame-free (user visual-verify).
Framework-wide change — regression-test Tomba 1 / MMX6 / Ape before any master merge.

---

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

Additional rule-outs (Ghidra raw-RAM analysis):
- 0x80050CE8 interior entry is SPURIOUS — Ghidra finds NO xrefs to it (over-detected
  by the recompiler; only fall-through reaches it). Harmless red herring, not the bug.
- No load-delay hazard in 0x50B08 (loads have compiler-scheduled nop delay slots).

SMOKING-GUN TRANSITION (overlay_native_ring): the repeating cycle is
0x89860 -> 0x89770 -> 0x50B08 -> 0x89788 -> [back to 0x89860]. After native 0x50B08's
FIRST jal (to 0x89788) returns (ra=0x50B30), control should continue to 0x50B08's
NEXT jal (0x85B20); instead it goes back to 0x89860 (InitHeap inside the 0x896E0
bootstrap) — i.e. the BOOTSTRAP RE-RUNS. So the 0x89788 return / the 0x50B30 CPS
continuation re-entry is mis-handled under native. This is consistent with why
BLOCKING 0x50B08 fixes it: running 0x50B08 in the interp BYPASSES the native CPS
continuation re-entry entirely (the interp runs jal callees as units and continues
sequentially), so the broken re-entry never happens.

REFINED HYPOTHESIS: the bug is in the CPS continuation RE-ENTRY for 0x50B08's interior
PCs — the trampoline dispatching pc=0x50B30 (an interior continuation, head<0) through
the g_psx_cps_mode path (overlay_loader.c ~line 1075: overlay_find_by_range +
generation/crc validation + entry-switch) across the 23 resident variants of the
0x38000 region. Either overlay_find_by_range picks the wrong variant for the interior
PC, or the re-entry doesn't route to block_80050B30, so control escapes back to the
bootstrap. This is a RUNTIME dispatch issue (overlay_loader.c), possibly compounded by
the multi-variant region, NOT necessarily a code_generator.cpp per-instruction bug.
NEXT: instrument the CPS continuation re-entry (which candidate/variant is chosen for
pc=0x50B30, and where c->fn(cpu) lands) — add it to the always-on native ring, OR
diff the interp's call-as-unit return vs native's trampoline re-entry at this exact
site. The 0x89788->0x89860 transition is the window to query.

GENERATED ENTRY-SWITCH CLEARED as the cause: checked all 5 resident _patched.c
variants with func_80050B08 — ALL include `case 0x80050B30`. The only inter-variant
diff is the SPURIOUS `0x80050CE8` case (present in 4, absent in 77548F26; harmless —
0x50CE8 has no xrefs). So re-entry at 0x50B30 would land at block_80050B30 correctly
IF the runtime dispatched pc=0x50B30 into func_80050B08. Since 0x85B20/0x50B30 never
execute, the runtime is NOT re-entering func_80050B08 for pc=0x50B30 at all — the
failure is in overlay_loader_dispatch's interior-continuation path (overlay_loader.c
~1075: head<0 -> g_psx_cps_mode -> overlay_find_by_range(0x50B30) -> crc-validate ->
cpu->pc=addr; c->fn). Across 43 resident 0x38000 variant DLLs, the candidate chosen
for the interior PC 0x50B30 (or its crc validation) is wrong, so control escapes to
the interp/bootstrap instead of block_80050B30. This is a RUNTIME dispatch bug
(multi-variant interior-continuation re-entry), to confirm with live instrumentation
of that exact path (log: for pc=0x50B30, the ci/variant overlay_find_by_range returns,
crc match y/n, and the dispatch return value).

KEY RING FACT: after 0x89788 returns (pc should be 0x50B30), NEITHER 0x50B30 (native
re-entry) NOR 0x85B20 (0x50B30's jal, via interp) ever appears in the native ring —
control goes straight to 0x89860 (bootstrap). So the trampoline neither native-re-
enters func_80050B08 nor interp-runs 0x50B30; pc=0x50B30 is routed somewhere that
lands back in the 0x896E0 bootstrap.

overlay_find_by_range is FIRST-BY-RANGE: it returns the first s_cand (registration
order) whose [range_lo, range_lo+range_len) contains phys, then crc-validates ONLY
that one (no fallthrough to other range-containing candidates). With 43 resident
0x38000 variant DLLs (+ interior-entry island fragments, c5efd70) all carrying
func_80050B08-range entries, the candidate chosen for interior PC 0x50B30 can be the
WRONG unit (a different variant or an island fragment whose range overlaps 0x50B30 but
whose entry-switch lacks 0x50B30 -> default -> runs from ITS top = wrong code), or one
whose crc fails -> return 0. Prime suspect for "control escapes to bootstrap."

FIX DIRECTION (runtime, overlay_loader.c — to design after live confirmation): make
interior-continuation re-entry select the candidate that genuinely OWNS the PC as a
continuation (entry-switch/known-continuation membership, or nearest function-start
<= phys whose body+crc covers phys), not merely first-by-range; and/or try all
range-containing candidates until one validates AND owns the continuation. Until then
the per-function native-disable (overlay_native_block) is the workaround.

STATUS: localized to the runtime interior-continuation dispatch; exact fault to be
confirmed by instrumenting the trampoline/overlay_find_by_range for pc=0x50B30.

## ROOT CAUSE CONFIRMED (2026-06-23, session 3) — break fall-through pollutes function range

Built a CPS-dispatch probe (overlay_cps_probe debug cmd: arm a watched interior PC,
records what overlay_find_by_range + validation decide). Live result for the spin:

  probe pc=0x89788: count=1021, chosen_addr=0x000896E0, ci=96, cands_in_range=6,
                    crc_matched=1, outcome=2 (ran native)

i.e. when func_80050B08 does `jal 0x89788` (its first subsystem-init call), the
trampoline can't find 0x89788 as a registered entry (idx_head<0), so the CPS path
runs overlay_find_by_range(0x89788). SIX loaded candidates have a range containing
0x89788; it returns **func_800896E0** (the crt0 bootstrap, crc 0x1D663E23, whose range
spans 0x896E0..past 0x89788). crc matches → it runs func_800896E0 with cpu->pc=0x89788.
0x89770 IS a case in func_800896E0's entry-switch (so the 0x89770 dispatch works), but
**0x89788 is NOT a case → default → func_800896E0 runs from its TOP** = the bootstrap
re-init (zero BSS, InitHeap [the 0xA0 spin], call main 0x50B08 again...). Infinite
re-loop → VBLANK stays masked → deadlock.

WHY func_800896E0's range covers 0x89788: function_discovery.cpp:493-499 treats BREAK
(and SYSCALL) as FALL-THROUGH ("BIOS uses these as in-line traps that return") and
pushes addr+4 onto the discovery work-queue. The bootstrap has `jal 0x50B08; break`
at 0x89780/0x89784 (break guards "main returned"). Discovery falls through the break
at 0x89784 into 0x89788 — a SEPARATE function (the init-array, called directly by
0x50B08) — absorbing it into func_800896E0's body/range. So 0x89788 is owned by
func_800896E0 (no matching entry-switch case) instead of being its own dispatchable
entry. The interp is immune: it executes 0x89788's bytes (init-array) directly, no
range ownership.

This is the SAME break-no-op flagged earlier; the damage is at COMPILE/DISCOVERY time
(range pollution), not runtime execution of break — which is why it survived the
"break isn't reached in the working path" rule-out.

### Fix options (framework-wide; needs regression on Tomba1/MMX6/Ape)
- A. BREAK terminates discovery (don't push addr+4). Clean root fix, matches
  strict_translator/interp/HW. RISK: regresses div-by-zero break (PsyQ relies on the
  break handler returning to EPC+4, i.e. fall-through) and any trap-that-returns.
- B. Clip a function's discovered range at the next discovered function entry, so even
  with break fall-through func_800896E0 stops at 0x89788. Preserves in-function
  fall-through (div-by-zero) while preventing cross-function swallowing. Likely the
  best fix; needs the next-entry set during range finalization.
- C. Fall through past BREAK only if addr+4 is NOT a known function entry (terminate
  when it would cross into another function). Targeted variant of A.
- D. Runtime: overlay_find_by_range prefer the candidate whose c->addr is the nearest
  start <= phys (most-specific entry) and whose crc matches, over first-by-range. Only
  helps if a candidate with addr==0x89788 (func_80089788) is loaded + crc-matches.
RECOMMEND B or C (don't change break execution semantics; stop the range from crossing
a function boundary). Workaround meanwhile: overlay_native_block 0x80050B08.

### CHATGPT-VALIDATED FIX PLAN (2026-06-23, debug principles shared)
Verdict: not a BREAK runtime-semantics bug — a discovery/range-ownership bug PLUS an
unsafe runtime "contained range implies callable entry" assumption. Fix BOTH layers
(defense-in-depth). Implement as a three-part class correction (all inside Arch A):

1. DISCOVERY FINALIZATION (B, required): clip a function's ownership range at the next
   STRONG known function entry:  owner_end = min(raw_end, next_strong_function_entry_after(start)).
   STRONG = direct jal target, direct j/tailcall target, executed dispatch root,
   game.toml symbol, manifest overlay entry. WEAK (do NOT clip on these) = linear-sweep
   artifact, speculative recovered host, unvalidated jump-table target, range-derived
   interior-only guess (e.g. the spurious 0x50CE8, which has no xrefs). 0x89788 is a
   direct jal target = strong, so clipping func_800896E0 at 0x89788 is valid.
   Compute next-entry from THAT candidate's OWN compile/discovery known-entry set —
   NOT from the runtime list of simultaneously-loaded variants (that would mix variants;
   overlapping scene variants each independently produce equivalent ownership).
2. DISCOVERY TRAVERSAL (C, good): BREAK and SYSCALL are EXCEPTION EDGES, not ordinary
   fall-through. Treat them the SAME (the distinction is NOT "syscall falls through /
   break terminates"). Enqueue addr+4 as a trap-RESUME edge only if addr+4 is not a
   strong known function entry. Neither may swallow a known function entry. (Preserves
   div-by-zero break / returning-trap idioms; prevents cross-function absorption.)
3. RUNTIME DISPATCH (D, required — fail-closed): add per-candidate LEGAL-ENTRY metadata
   (the entry-switch case set / legal continuation PCs). overlay_find_by_range becomes:
   (a) find candidates whose owner_range contains phys, (b) CRC/byte validate,
   (c) REQUIRE candidate.legal_entries contains phys, (d) prefer most-specific owner if
   several remain, (e) if none legally serves phys -> sanctioned dirty-RAM interpreter.
   This does NOT violate Arch A (fallback is the sanctioned interp, not a stub/HLE).
   Correct degradation = interp runs the exact live bytes at that PC.
PLUS codegen: the generated entry-switch DEFAULT must NOT fall through to the top block.
   Emit `default: return psx_native_bad_entry(cpu, unit_id, cpu->pc);` — dev/strict =
   abort with ring context (matches "fully implemented or aborts"); normal runtime =
   reject provider and fall to sanctioned dirty interp. It must NEVER run from the top.
NUANCE: keep dispatch OwnerRange (for range-based containment) separate from
   CRC CoveredRanges (bytes the native body may execute + validate). Don't conflate
   "clipped ownership range" with "the only bytes this function may execute."

INVARIANT (the takeaway): "containment is not ownership, and ownership is not entry
legality." Expected post-fix: func_800896E0 owns [0x896E0,0x89788); 0x89788 either has
its own native candidate with an exact legal entry or falls to dirty interp;
overlay_find_by_range(0x89788) can neither choose func_800896E0 nor run it from the top.

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

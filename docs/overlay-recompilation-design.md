# Overlay Recompilation — Design Review

**Status:** CONVERGED (post-review). The open questions below were taken to an
external reviewer; the resolutions are recorded in §5–§7 at the end, which
supersede any "OPEN" markers above. §0–§4 are preserved as the reasoning trail.

---

## 0. Context (for a reader unfamiliar with the project)

PSXRecomp is a **static recompiler** for PlayStation 1 games: it translates the
game's MIPS machine code to native C ahead of time, compiles that to a real
binary, and runs it natively (no CPU interpreter on the hot path). This is the
N64Recomp / SuperMarioWorldRecomp model. The project premise — non-negotiable —
is that **the game runs as statically recompiled native code**, not emulation.

The hard part: PS1 games stream **overlays** — code segments DMA'd from disc
into RAM at runtime, executed, then overwritten by the next overlay. A pure
static recompiler can't see overlay code at build time (it isn't in the main
executable; it arrives at runtime). Two existing mechanisms:

1. **Dirty-RAM interpreter** (`dirty_ram_interp.c`): a small MIPS interpreter
   that runs any code in RAM pages written at runtime. This is the *failover* —
   correct but slow, and explicitly **not** the goal.
2. **Overlay DLL cache** (the subject of this doc): capture overlays as they
   load, recompile them offline to native DLLs keyed by content hash, and on
   later runs `LoadLibrary` the DLL and dispatch its functions **before** the
   interpreter. Coverage grows as people play.

### The pipeline as built
```
runtime capture  ──► overlay_captures.json (full bytes + executed PCs + dispatch PCs)
       │                         │
       │                  compile_overlays.py (offline)
       │                         │  - classify seeds (which PCs are real function entries)
       │                         │  - run the recompiler on the overlay bytes
       │                         │  - audit generated C, compile to <crc>.dll
       ▼                         ▼
  dirty-RAM page          cache/<game_id>/<region>_<crc>.dll
       │                         │
       └──► dispatch: overlay DLL func (if registered) ──► else interpreter (failover)
```

---

## 1. The governing principle we've converged on

**Asymmetric failure ⇒ bias for precision over recall.**

- A **missed** overlay function is *safe*: the loader only claims the specific
  `func_XXXXXXXX` addresses it registered from the DLL. An unregistered address
  returns "not handled" and **falls through to the interpreter**. The seed is
  then captured and the overlay is regenerated next time with that seed
  included. Under-coverage self-heals.
- A **mis-compiled** function is *fatal*: it gets registered, the game calls it,
  and it executes wrong native code against shared CPU state → corruption →
  black/blue screen. There is no failover once we've claimed an address.

Therefore every design choice should **minimize false registrations**, even at
the cost of compiling fewer functions per pass. Recall is recovered for free by
the interpreter-failover-and-recapture loop; precision is not recoverable.

**Question for review:** Is this asymmetry actually total? Are there cases where
a *missed* function does NOT safely fall back — e.g., the game's control flow
assumes the function executes within some timing budget that the interpreter
blows, or the missed function is on a path where the interpreter itself can't
handle some instruction? If "miss is always safe" has exceptions, the whole
precision-first thesis weakens.

---

## 2. Converged positions (please attack)

### 2.1 Capture is complete and data-inclusive; the DLL is code-only
- The on-disk capture artifact contains the **full overlay image, including
  data** (jump tables, constant pools). Required because (a) re-recompiling
  later with better heuristics needs the original bytes, (b) the recompiler must
  *read* data to resolve jump tables, (c) the cache key is a CRC over the image.
- The compiled **DLL contains code only**. The overlay's data is never emitted;
  it lives in PSX RAM (the game DMA'd it there), and recompiled functions read
  it through normal memory callbacks at runtime.
- Consequence: "how much benign data surrounds the code" is **not** a
  correctness question for the DLL. The only correctness question is "is this
  word reached as code?" Data is recognized as not-code and left alone.

### 2.2 Capture bytes: DMA-time vs execution-time (OPEN — leaning a direction)
The current capture stores **DMA-time ("unpatched")** bytes — the overlay as
delivered from disc, *before* any load-time relocation/fixups the game applies.
If a game patches the overlay after load (relocation, jump-table base fixups,
self-modification), the bytes we recompile ≠ the bytes that actually executed,
and the DLL is subtly wrong. This is a candidate root cause for past black/blue
failures that has nothing to do with seed classification.

- **What's correct to recompile from:** execution-time bytes (what the CPU
  actually ran).
- **Why not just snapshot RAM at exit:** by exit, that RAM range may already be
  contaminated by a *later* overlay reusing the address.
- **The right moment is first-execution-time:** snapshot `RAM[region]` the first
  time we dispatch into it — after fixups, before reuse.
- **Proposed:** in dev builds, dump **both** (`<crc>_dma` and `<crc>_exec`) and
  diff them. Identical ⇒ no runtime patching, capture-time is irrelevant.
  Different ⇒ the diff reveals the fixup pattern and likely the bug.

**Question for review:** Is "recompile from execution-time bytes" correct, or
does it break the content-hash cache model (the patched bytes may be
load-address-specific — see §2.5 relocation)? Should the cache key be over
unpatched or patched bytes?

### 2.3 Function discovery is one evidence hierarchy; no whole-byte sweep
Rank evidence that an address is a real function entry:
1. Known entries (initial PC, exported symbols, declared entries)
2. **Executed PCs** — we literally watched them run (strongest signal)
3. Direct `JAL` targets (statically provable callers)
4. Jump-table targets we resolved by reading the table
5. ~~Whole-byte sweep: decode every byte hoping to find functions~~

Tier 5 is the *only* unsound tier for overlays — it invents entries from data,
which is what produces the data-as-code failures. We have tiers 1–4 for free on
overlays (we captured them by running them). So discovery should use 1–4 and
**never** tier 5 for overlays. (The cold main EXE, with no execution evidence,
is the only place tier 5 has ever been justified — and even there it should be
the flagged, lowest-confidence tier.)

This was previously implemented as an opt-in recompiler flag
(`--overlay-exact`). We believe a flag is the wrong abstraction: evidence-driven
discovery is *implied by whether execution evidence exists for the input*, not a
human toggle.

### 2.4 Mid-function seeds: capture them, merge at encode time
A runtime "dispatch entry" can be a **jump-table case label mid-function**, not
a callable function boundary. These are dangerous: the game *does* reach them
(so we'd register them), but compiling them as standalone functions yields wrong
boundaries/stack assumptions.

- Don't filter at capture time (the runtime can't tell). **Capture everything.**
- At encode time, with the full bytes, classify each seed: a seed that falls
  strictly inside another function's `[start,end)` is **mid-function → merge
  into the parent** (it becomes a label in the parent's CFG, not its own func).
- The discriminators are local and don't need external callers: preceded by
  `jr $ra`+delay slot (real boundary), has a prologue, is a direct `JAL` target.

### 2.5 Confidence flagging ("jump-risk")
The ambiguous residue: a function reachable **only** via a computed/indirect
jump (jump table or function pointer), never via a direct `JAL`. It looks
identical to a jump-table case label. Proposal: tag each emitted entry with a
confidence (`strong` / `mid-function` / `jump-risk` / `forced`) in a
machine-readable sidecar (the recompiler already emits `skipped_functions.json`,
so the pattern exists). 

**Open sub-question:** for `jump-risk` entries, do we (a) emit them with the
flag, or (b) *exclude* them from compilation initially and let them
interp-fallback until a direct-JAL or execution proof confirms them? Given the
precision-first thesis, (b) is tempting.

### 2.6 Overlay segmentation and cache keying (OPEN)
The runtime currently captures **one region = all contiguous dirty pages**
(observed: a single 356 KB region for Tomba). That is almost certainly *not one
overlay* — it likely merges several disc loads plus unrelated dirtied RAM.
Problems: the cache is CRC-keyed per region, so if a later run loads a different
overlay into part of that range, the CRC changes and the whole region misses.
Proposal: re-segment using the CD-DMA transfer log (each forward CH3 DMA records
LBA→dest→size) into real per-load overlays, and key the cache per overlay.

**Question for review:** can disc-load boundaries (CD-DMA log) reliably bound
overlays, given overlapping/reused address ranges and overlays assembled from
multiple DMA blocks? Is per-(load_addr, crc) keying sufficient, or does
relocation (§2.2) force something else?

### 2.7 Code hygiene (DRY)
The same low-level MIPS primitives (`jr $ra` detect, `addiu $sp` prologue,
valid-MIPS-word, classify-control-flow) are currently reimplemented in at least
three places (the Python seed vetter, the Python overlay classifier, the C
runtime decodability guard). The *policies* differ legitimately (single-address
verdict vs. whole-overlay CFG classification) and should stay separate, but the
primitives should be one shared module.

---

## 3. The specific failure we just observed (concrete grounding)
- Captured one overlay: load `0x800E7000`, 356 KB, 15358 executed PCs, 66
  dispatch-entry seeds, full bytes present.
- The offline classifier selected 122 function entries + 59 direct-JAL targets
  and excluded 3096 branch targets + 14221 observed-only PCs (mid-function
  filtering already works).
- But the recompiler, run **without** evidence-scoping (whole-byte sweep), walked
  into embedded data tables and emitted `UNKNOWN_BAD` jump targets +
  `UNSUPPORTED_INSTRUCTION` runs. The offline audit then refused to emit the DLL
  (all-or-nothing per region) → **zero DLLs produced**.
- So the observed failure is at the **discovery strategy** (tier-5 sweep
  decoding data), not at seed classification. This is the direct evidence behind
  §2.3.

---

## 4. The specific questions we most want challenged
1. Is the precision-over-recall asymmetry (§1) truly total, or are there
   "missed function does not safely interp" cases?
2. Execution-time vs DMA-time capture (§2.2): which should we recompile from,
   and which should key the cache, especially under relocation?
3. Does relocation/self-modification break the static-overlay premise for some
   overlays — i.e., is there a class that *must* stay interpreted forever, and
   how do we detect it cheaply?
4. Jump-risk entries (§2.5): emit-with-flag vs exclude-until-proven?
5. Segmentation (§2.6): can the CD-DMA log reliably carve real overlay
   boundaries out of the dirty-page blob?

---

## 5. Resolved — converged decisions (post-review)

Accepted refinements (now design-of-record, not open):

- **(A) Runtime guard = write-invalidation, not CRC-per-dispatch.** On DLL
  load: validate code + dependency ranges once. On any write to a *watched*
  RAM page: invalidate the affected registered entries / DLL range. On
  dispatch: registration-table check only, no hot-path CRC. Coarse page-level
  invalidation first; false positives (over-invalidation → interp fallback)
  are acceptable. Do NOT start with fine interval invalidation.
- **(D) Internal labels are compiled inside a proven parent, never
  registered.** A jump-table / branch target is *not* a callable function
  entry; it is compiled as a label inside a call-proven parent function and
  reached via the parent's native switch. This is what keeps precision-first
  from degenerating into "interpret all switch code."
- **(B) Edge capture, minimal schema** — at the control-flow classifier, not
  the dispatcher. `{src_pc, dst_pc, edge_type ∈ {Jal,Jalr,Jump,Branch,JrRa,
  JrOther}, writes_ra}`. Defer `sp_before/after`, `delay_slot_pc` until a
  concrete classifier failure demands them. Share the edge enum with the
  recompiler's static walk.
- **(C) `UNSUPPORTED_INSTRUCTION` is a boundary/CFG suspicion first, real
  opcode gap last.** Classify in order: (1) reachable from entry via valid CF?
  (2) fell through past `jr $ra`/terminal branch into data? (3) in a reachable
  branch/jump delay slot? (4) impossible target from bad discovery? (5) only
  then a true opcode gap. Trim only when justified by an actual terminal
  control-flow edge — never "trim until the audit passes."
- **(E) Native-vs-interp differential testing** is the empirical backbone for
  the parity assumption — but it is increment-2, not an increment-1 blocker.

### The three-bucket entry rule
```
direct JAL target / jalr-with-link / declared / initial entry
        -> compile as function, REGISTER as callable
jump-table or branch target inside a known parent CFG
        -> compile as label inside the parent, DO NOT register
observed dispatch target with no call-edge / parent proof
        -> DO NOT register; interp fallback; capture better edge evidence
```

### The load-bearing invariant
```
Compiled  != Registered.
Emitted   != Callable.
Only REGISTERED entries may bypass the interpreter.
```
The DLL may physically contain functions for diagnostics/future use; what the
runtime registration table claims is the only thing that affects correctness.

### Precision-over-recall — refined (answer to §4 Q1)
The asymmetry is not a law; "a miss is safe" holds only if native↔interp state
preservation, instruction coverage, delay/load-slot and IRQ/DMA-timing
equivalence, and stale-registration invalidation all hold. But the thesis
survives in its correct form: **false registration is immediately corrupting;
missed registration is recoverable if interpreter parity holds; therefore
optimize for registration precision.** Parity itself is to be *measured* (E),
not assumed.

## 6. RAM-write-path audit result (gates the §5-A guard)

- All CPU stores (generated + interpreter) and DMA writes funnel through
  `psx_write_{word,half,byte}` in `runtime/src/memory.c`. The only `memcpy`/
  `memset` into memory target device-local buffers (`param_fifo`, `vram`,
  `spu_ram`), never main `ram`. **No native path mutates main RAM behind the
  store functions** ⇒ write-invalidation has no hidden false-negative source.
- Existing dirty tracking on the store path is **kernel-only**
  (`dirty_ram_mark_kernel_write` bails at `phys >= 0x10000`). Overlay pages are
  marked only by DMA (`dirty_ram_mark_executable_range`). So overlay CPU
  self-modification is currently untracked; the bitmap covers all 2 MB, the
  guard simply doesn't set it there.
- **Required change for §5-A:** a watched-overlay-pages bitmap set at
  registration; one extra bitmap test in the RAM-store branch of all three
  `psx_write_*`; on hit → `overlay_loader_invalidate_page()`. Coarse, cheap.

## 7. Implementation increments

**Increment 1 — make DLLs safe and loadable (current)**
1. No whole-byte sweep for overlay inputs (mandatory, ingester-set `--overlay`
   contract; not a human flag).
2. Split classification: code-reachable / callable entry / internal label.
3. Per-function (or SCC) audit, not per-region.
4. Register only proven callable entries.
5. Jump-table/case labels compiled only inside their proven parent.
6. Coarse write-invalidation for registered overlay pages (per §6).
   Success: Tomba emits a partial DLL; runtime loads it; only strong entries
   registered; unproven code interp-falls-back; no black/blue from false
   registration.

**Increment 2 — make it measurable**
Edge-capture schema (B); DLL manifest with code/dependency ranges;
registration-time validation; native-vs-interp differential harness (E).

**Increment 3 — make it efficient and stable**
Execution-time vs DMA-time dual capture; source-grouped + exec-specialized
cache key; executed-code-envelope segmentation; CD-DMA as provenance only.

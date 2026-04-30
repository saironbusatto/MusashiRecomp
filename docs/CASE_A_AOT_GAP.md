# Case A: memcard work blocked on BIOS RAM relocation/AOT support

**Date:** 2026-04-30
**Decision:** stop memcard symptom debugging; build trace-generated AOT RAM-code manifest.
**Update (2026-04-30, post seed-add):** vertical slice partially landed — see "Vertical slice result" at bottom.
**Predecessor:** every memo named `phase4_*real*root_cause*` is downstream of this gap.

---

## What we found

For ~3 weeks the memcard subsystem in v4 has produced an unbroken chain of
"real root cause" memos that get superseded a few sessions later.  Today's
verification proved why.

### Beetle wtrace (ground truth)

A new RAM-write trace was added to Beetle PSX (mednafen-psx CPU,
SB/SH/SW handlers fire `g_psxrecomp_wtrace_cb` after each write).  Default-armed
on the card-gate window `[0x7568..0x756C)`.  After driving CROSS, Beetle
captured 145 writes touching `0x7568` / `0x7569`.  Every writer PC lives in
**RAM kuseg** (< `0x00800000`) — i.e. dynamically installed code, not BIOS ROM.

| Writer PC (RAM) | Value written | Caller `$ra` |
|---|---|---|
| `0x00005EF4` | **0x02** (LSB-clear → "OPEN, run chain") | `BFC08{B98,D08,F28}` (ROM thunks) |
| `0x00005FA8` | **0x04** (LSB-clear → "OPEN, alt state") | `BFC08C94` |
| `0x00004F54` | 0x01 (LSB-set → "BUSY, busy state") | `0x00004F0C` (RAM) |
| `0x00005DD0`, `0x00005DD8` | 0x01 (init) | `0x00005DB8` (RAM) |

### Recomp execution (the gap)

Same window, recomp side:

- `dispatch_check` over those PCs: `0x445C` and `0x4D6C` are statically
  dispatched, but **0x4F0C / 0x4F54 / 0x5DB8 / 0x5DD0 / 0x5DD8 / 0x5EF4 /
  0x5FA8 / 0x6524 → not dispatched, ever**.
- `dirty_ram_interp` per-PC histogram (post-CROSS, 6 s window):
  reaches `0xCF0`, `0x52xx` series, `0x5674`, and `0x6{414..594}` —
  but **none of the Beetle writer PCs**.
- Armed wtrace on `[0x7568..0x756C)` over the same press window:
  **0 writes**, despite +2.1M dispatches in the window.
- Interpreter aborts: +1450 in the window — separate failure mode at
  *other* RAM PCs, not the gate writer's blocker.

### Selective coverage pattern

Recomp's RAM-installed code surface IS partially populated:

```
Reached by dirty_ram_interp:   0xCF0, 0x52xx, 0x5674, 0x64xx, 0x65{0C,14,88,8C,94}
NOT reached (Beetle writers):  0x4F0C, 0x4F54, 0x5DB8/D0/D8, 0x5EF4, 0x5FA8
```

Two RAM windows are dark in our recomp:
**0x00004F00..0x00004FFF** and **0x00005800..0x00005FFF**.

That selective miss is the diagnostic signature of **install/dispatch path
gaps**, not interpreter bugs.  Whoever in the BIOS copies code into 0x4F00
and 0x5800 is either not running, or running but the recompiler's install
hook doesn't recognize the copy as code installation.

---

## Why every prior "root cause" was a phantom

If the chain handler at `0x5EF4` never executes in our recomp, every
downstream symptom — chain stops at byte N, slot toggle wrong,
`[0x755A]` abort flag, `[0x7568+slot]` not LSB-clear, sectors 1..15
never requested, accumulator stuck at 0x3F instead of 0xC3 —
is a consequence of "the writer code never ran", not its own
independent bug.

The memo trail (slot 0 chain real structure → D3→D4 transition →
chain resets after 16 bytes → real blocker → setup writes → Layer 5
IRQ delivery → ...) reads exactly like investigating consequences of
a missing foundation layer.  Every memo was correct at the time about
what it observed; none of them saw the layer below.

---

## What the project plan already says

`PLAN.md:941-967` ("BIOS Relocation Is a Hard Gate") already mandates
this work:

> ROM↔RAM aliasing must be solved before full BIOS recompilation. The PS1
> BIOS copies code from `0xBFCxxxxx` to `0x800xxxxx` during init and runs
> the RAM copy. A recompiler that does not understand this will silently
> emit two C functions for the same logical BIOS routine, under different
> names, with different identities, and every dispatch table built later
> will be wrong in a way that is almost impossible to debug.

`PLAN.md:933` lists `address_aliases.json` (backed by
`relocation_proofs/`) as a Phase 1 exit gate.  Both files exist on disk —
the **primary copy alias was implemented** in a prior session
(`generated/address_aliases.json`: 69 ROM↔RAM aliases for the single
proven copy ROM 0xBFC10000..0xBFC18BEF → RAM 0x500..0x90EF).  What was
missing: *additional entry points inside the copied region that aren't
Ghidra-known function starts.*  These reach the recompiler via
`recompiler/seeds/dispatch_miss_seeds.json`.

`CLAUDE.md:333-365` (Rule 18) authorizes `dirty_ram_interp` for
truly *assembled-at-runtime* stubs (e.g. the 4-instruction SIO
data-byte handler at RAM 0xCF0 — which today's data confirms IS
running).  But the bulk of "dynamically installed RAM code" is
ROM-copied, not assembled in-place; that bulk belongs to the
relocation framework, not the interpreter.

---

## Decision

**Stop memcard symptom debugging.**  Implement trace-generated AOT
RAM-code support as a vertical slice:

1. Define minimal manifest schema.
2. Capture proof artifacts via Beetle.
3. Teach the recompiler to ingest the manifest.
4. Add runtime hash check before dispatching AOT RAM functions.
5. Re-test memcard.

**Do not** hand-clear `[0x7568+slot]`, write a C shim, add HLE, or add
a runtime JIT.

---

## Vertical slice result (2026-04-30, post seed-add)

The plan above was followed but collapsed dramatically when we discovered
the Phase 1e infrastructure already exists and the right hook is the
existing `dispatch_miss_seeds.json`.  Five seeds added:

| Seed (ROM) | Becomes (RAM) | Purpose |
|---|---|---|
| 0xBFC14A54 | 0x00004F54 | chain BUSY-writer |
| 0xBFC158D0 | 0x00005DD0 | chain init slot-0 BUSY |
| 0xBFC158D8 | 0x00005DD8 | chain init slot-1 BUSY |
| **0xBFC159F4** | **0x00005EF4** | **chain step 0x02-OPEN writer (KEY)** |
| **0xBFC15AA8** | **0x00005FA8** | **chain step 0x04-OPEN writer (KEY)** |

After regen + rebuild:

- ✓ All 5 standalone functions emitted (e.g. `func_00005EF4` is a 3-instruction leaf: `sb t3, 0(a3); addiu v0, zero, 1; jr ra`).
- ✓ All 5 in `dispatch_table`.
- ✗ **None of the 5 are dispatched at runtime — chain still doesn't reach them.**
- ✗ `[0x80007568]`, `[0x80007569]` still all-zero post-CROSS.
- ✗ `[0x80007510..0x00007570]` chain state region all-zero — **chain init function `FUN_bfc158a8` (= func_00005DA8) never runs in our recomp, regardless of seeds**.

### What this proves

The "writer is unreachable" gap had two layers:
1. **Writer PC missing from dispatch table** — fixed by these 5 seeds.  Confirmed.
2. **Caller chain (init + chain coordinator) never runs in our recomp** — independent issue, surfaced by the validation.

The seeds are correct prerequisites. They unblock layer 1.  Layer 2 is
the actual remaining blocker: the BIOS shell is on the memcard screen
(`[0x80066948] = 0x32` confirms MEMORY CARD selected) but `FUN_bfc14b00`
(chain coordinator, `func_00005000`) is never dispatched.  The memcard
chain subsystem has never been initialized in our recomp during this
session — gate bytes have never been written, even to 0x01 (which the
init function writes at the very first call).

The Beetle wtrace also revealed a misread on my part: the writer at
RAM 0x5EF4 is a tiny leaf utility (write byte through pointer), called
from many BIOS sites — not specifically a chain-step jump-table target.
The chain step jump tables at RAM 0x6c70 / 0x6c98 / 0x6ccc point at
addresses in the 0x52xx / 0x56xx-0x5Axx / 0x5Bxx-0x5Dxx ranges, NOT at
0x5EF4 / 0x5FA8.  So the writers aren't reached via the chain step
dispatcher — they're reached via direct calls from BIOS code at
`BFC08{B98,C94,D08,F28}` (`$ra` in the Beetle wtrace).  Those BIOS
caller sites must execute for our writers to be reached.

### Vertical slice status

- AOT/seed work for the writer PCs: **done and committed**.
- Validation: **partial**.  Writers exist; callers don't run.
- Next blocker: why the BIOS chain init / coordinator never runs in our
  recomp.  This is an upstream issue from the writer-emission fix and
  needs its own diagnostic pass — explicitly NOT "memcard symptom
  debugging" because the symptom (chain init never runs) is a different
  category from "chain runs but fails on byte N".

---

## Verification reproducer

```bash
# 1. Build & launch Beetle oracle.
PATH=/c/msys64/mingw64/bin:$PATH cmake --build runtime/build --target psx-beetleoracle -j8
taskkill //F //IM psx-beetleoracle.exe 2>/dev/null
PATH=/c/msys64/mingw64/bin:$PATH ./runtime/build/psx-beetleoracle.exe bios/SCPH1001.BIN &
python tools/probe_beetle_wtrace_cross.py    # ground-truth writer PCs

# 2. Build & launch recomp.
PATH=/c/msys64/mingw64/bin:$PATH cmake --build runtime/build --target psx-runtime -j8
taskkill //F //IM psx-runtime.exe 2>/dev/null
PATH=/c/msys64/mingw64/bin:$PATH ./runtime/build/psx-runtime.exe &

# 3. Validate.
python tools/verify_with_cross.py            # should now show func_00005EF4 in dispatch table
python tools/check_chain_init.py             # surfaces "chain init never ran" upstream blocker
```

Expected vertical-slice outcome: dispatch table contains
`{0x00005EF4u, func_00005EF4}` (✓ confirmed today); 
recomp-side `dispatch_check 0x00005EF4` returns `True` *if and when*
the chain coordinator runs (✗ blocked today on upstream init issue).

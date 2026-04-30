# Case A: memcard work blocked on BIOS RAM relocation/AOT support

**Date:** 2026-04-30
**Decision:** stop memcard symptom debugging; build trace-generated AOT RAM-code manifest.
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
`relocation_proofs/`) as a Phase 1 exit gate.  Neither artifact exists
on disk today.  The work is overdue, not speculative.

`CLAUDE.md:333-365` (Rule 18) authorizes `dirty_ram_interp` for
truly *assembled-at-runtime* stubs (e.g. the 4-instruction SIO
data-byte handler at RAM 0xCF0 — which today's data confirms IS
running).  But the bulk of "dynamically installed RAM code" is
ROM-copied, not assembled in-place; that bulk belongs to the
relocation framework, not the interpreter.

---

## Decision

**Stop memcard symptom debugging.**  Implement trace-generated AOT
RAM-code manifest support as a vertical slice:

1. Define minimal `generated/address_aliases.json` schema.
2. Capture proof artifacts via Beetle (RAM bytes + hash + entrypoints
   + writer/first-exec PCs) for the two dark windows.
3. Teach the recompiler to ingest the manifest and emit functions at
   the manifest's RAM destination addresses.
4. Add runtime hash check before dispatching AOT RAM functions.
5. Re-test memcard.  Either it works (validating the AOT path) or the
   next failure points at something further down (e.g. interp
   semantics in code we now reach but mishandle).

**Do not** hand-clear `[0x7568+slot]`, write a C shim, add HLE, or add
a runtime JIT.  The phantom-fix history is exactly what those would
extend.

---

## Verification reproducer

```bash
# 1. Build & launch Beetle oracle (its wtrace is default-armed on 0x7568..0x756C).
cd beetle-psx && make platform=mingw_x86_64 STATIC_LINKING=1 HAVE_LIGHTREC=0 -j8
cp mednafen_psx_libretro.dll libmednafen_psx.a && cd ..
PATH=/c/msys64/mingw64/bin:$PATH cmake --build runtime/build --target psx-beetleoracle -j8
taskkill //F //IM psx-beetleoracle.exe 2>/dev/null
PATH=/c/msys64/mingw64/bin:$PATH ./runtime/build/psx-beetleoracle.exe bios/SCPH1001.BIN &

# 2. Drive CROSS in Beetle, dump writers (oracle-side ground truth).
python tools/probe_beetle_wtrace_cross.py

# 3. Build & launch recomp.
PATH=/c/msys64/mingw64/bin:$PATH cmake --build runtime/build --target psx-runtime -j8
taskkill //F //IM psx-runtime.exe 2>/dev/null
PATH=/c/msys64/mingw64/bin:$PATH ./runtime/build/psx-runtime.exe &

# 4. Verify recomp: arm wtrace on 0x7568..0x756C, press CROSS, query everything.
python tools/verify_with_cross.py
```

Expected pre-AOT outcome (today): recomp `dispatch_check` returns
`False` for `0x5EF4`, `0x5FA8`, `0x4F54`; recomp wtrace shows 0 hits at
`0x7568..0x756C`; `dirty_ram_interp` histogram contains 0xCF0 / 0x52xx /
0x6xxx but no entry in `0x4F00..0x4FFF` or `0x5800..0x5FFF`.

Post-AOT success criterion: recomp `dispatch_check` returns `True` for
`0x5EF4` and `0x5FA8`; wtrace captures LSB-clear writes to `0x7568` /
`0x7569` matching Beetle's transitions.

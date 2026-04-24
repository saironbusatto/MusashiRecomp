# PSXRecomp v4 — Rules

This file is the constitution for v4. Read it at the start of every
session before doing any work.

---

## 0. The architecture is locked

v4 implements **Architecture A**: static MIPS-to-C recompilation of
`bios/SCPH1001.BIN`, producing native C that links into the runtime as
real compiled functions.

There is **no MIPS interpreter** in v4 for the BIOS path. Not as a
fallback. Not as a "temporary" measure. Not for "code we couldn't
recompile yet". If a BIOS function cannot be recompiled, the recompiler
is wrong and must be fixed. The interpreter does not exist. Do not
write one.

There is **no HLE BIOS layer** in v4. No `bios.c` with case branches
intercepting A0/B0/C0 vectors. No C reimplementations of `OpenEvent` or
`StartCard` or `alloc_kernel_memory`. The BIOS IS the recompiled C
output of `SCPH1001.BIN`. If a BIOS routine misbehaves, the answer is
to fix the recompiler or fix the hardware simulation it touches via
MMIO — never to write a C "shim" that produces the answer the BIOS
would have produced.

There are **no stubs**. A function is either fully implemented or it
aborts with a fatal error. `return 0;`, `return 1;`, `cpu->v0 = 1;
return;` are all stubs. `// TODO`, `// FIXME`, `// for now` are all
stubs. Hand-delivering an event because the chain handler isn't
installed is a stub wearing a costume and is the worst kind because it
hides the missing integration.

If you find yourself wanting to violate any of the above three
paragraphs, **stop and re-read PLAN.md**. Every prior attempt failed by
violating exactly these rules under pressure.

---

## 1. The BIOS is recompilation target #1, the game is target #2

Phase 1-3 of PLAN.md exist to get the BIOS recompiled and booting on
its own. The BIOS must reach the Sony logo and the BIOS shell, running
entirely as native C, before any game work begins. There is no path
that loads a game EXE before the BIOS is fully working in v4. **Do not
load a game ISO. Do not load a game EXE.** Tomba does not exist in v4
until Phase 5.

If you find yourself needing to load a game to "test something",
whatever you're testing belongs to a phase that hasn't started yet.

---

## 2. DuckStation is the oracle, always

Truth comes from two sources:

- **Ghidra** for what the BIOS code is supposed to do (static analysis
  of `SCPH1001.BIN` loaded at `0xBFC00000`)
- **DuckStation** at `F:/Projects/psxrecomp-v4/duckstation/build/bin/duckstation-qt.exe`
  for what real PS1 hardware does at runtime (dynamic oracle). The `duckstation/`
  directory is a **git submodule** pinned at upstream `stenzek/duckstation`
  commit `ffb33c281` (release-20260328 era). Our PSXRecomp TCP debug server
  patch lives at `tools/duckstation/psxrecomp_oracle.patch` and auto-applies
  via `tools/duckstation/setup.sh`. Never commit changes into the submodule —
  edit files freely for local experimentation, then regenerate the patch with
  `git -C duckstation diff <base> > tools/duckstation/psxrecomp_oracle.patch`

Use both, never just one. Don't guess. Don't say "probably". If you
cannot answer a question from Ghidra or DuckStation, the answer is
"I don't know yet" — not a confident guess.

---

## 3. No printf debugging. No log files. Ever.

If you need to inspect runtime state, **build a TCP debug server
command** for it. The v3 build accumulated 555 GB of `boot_trace*.log`
and `card_test*.log` files because previous sessions used `fprintf` for
"just this one thing". The rule is absolute: **no `fprintf(stderr, ...)`
in source code, ever, for any reason.**

When the v4 runtime is built (Phase 2+), it will have a TCP debug
server on a fresh port. All inspection goes through that.

---

## 4. Never modify generated code

The output of the recompiler — files in `recompiler/output/` or
`generated/SCPH1001_full.c` etc. — is a build artifact. If the
generated code is wrong, the fix is in the recompiler source
(`recompiler/src/code_generator.cpp` and friends), not in the
generated file.

This is the same rule as v3 had, and it stays.

---

## 5. Don't accept partial milestones

Phase completion requires the user-visible end state, not "I think it
should work now". Phase 3 is "Sony logo displays on screen". Not "the
recompiler emitted code that probably draws the logo". Not "the GPU
command stream looks right in the debug server". **The pixels appear
on screen, or the phase is not done.**

This was the v3 failure mode: declaring "memory card screen freeze
RESOLVED" when in fact the screen had been unlocked by hand-delivering
a fake event. The fake delivery was not progress, it was theater.

---

## 6. Session start checklist

At the start of every session, before any code change:

1. Read this file (CLAUDE.md).
2. Read PLAN.md to confirm what phase we are in and what the next
   concrete milestone is.
3. Verify Ghidra MCP is reachable. If not, stop and ask.
4. Verify DuckStation builds and launches with `-bios`. If not, stop
   and ask.
5. State out loud: "Architecture A is locked. No interpreter. No HLE.
   No stubs. BIOS first. Game never until Phase 5."

If any of these fail, do not proceed with the user's task — surface
the failure first.

---

## 7. Salvage from v3 — what's allowed and what's not

The recompiler in `recompiler/` was salvaged from v3 because the
core MIPS-to-C translator pieces (`basic_block.cpp`, `control_flow.cpp`,
`function_analysis.cpp`, `mips_decoder.cpp`, `code_generator.cpp`)
operate on raw MIPS bytes and have nothing wrong with them. They just
need a new entry point that ingests a flat ROM at `0xBFC00000` instead
of a `PS-X EXE`-headered file, plus extensions to `code_generator.cpp`
to handle COP0 kernel-mode instructions the BIOS uses.

**The runner from v3 was not salvaged.** Specifically:

- `bios.c` (1808 LOC HLE shims) — discarded
- `interpreter.c` (919 LOC MIPS interpreter) — discarded
- `events.c`, `threads.c` — discarded (recompiled BIOS manages its own EvCB/TCB)
- `bios_trace.c`, `func_logger.c` — discarded (interpreter-era helpers)
- `main_runner.cpp` — discarded (drove the interpreter)

The hardware simulation files from v3 (`memory.c`, `gpu.c`,
`gpu_sw_renderer.c`, `dma.c`, `interrupts.c`, `timers.c`, `sio.c`,
`memcard.c`, `cdrom.c`, `iso_reader.cpp`, `gte.cpp`, `spu.c`,
`debug_server.c`) are **eligible for salvage in Phase 2** when v4
needs them, but they will be copied in **one at a time**, audited for
HLE-state-leakage and stub patterns first, and only the parts that are
hardware simulation (not BIOS state simulation) are kept.

**Do not bulk-copy `psxrecomp/runner/src/` from v3.** Doing so will
re-import the disease.

---

## 8. Reference the right project for examples

PSXRecomp v4 is a sibling project to:

- **N64Recomp** (RT64 team) — proven static recompilation model for N64
- **SuperMarioWorldRecomp** (`F:/Projects/SuperMarioWorldRecomp/`) — sibling SNES recomp
- **SuperMarioWorldRecomp-oracle** (`F:/Projects/SuperMarioWorldRecomp-oracle/`)
- **NESRecomp** — referenced in v3's debug_server.c comments

When you need to know "how does a recomp project handle X?", read those
projects. **Do not** look at v1 (`F:/Projects/psxrecomp/`) or v2
(`F:/Projects/psxrecomp-v2/`) or v3 (`F:/Projects/psxrecomp-projects-v3/`)
for architectural guidance. They are reference for what failed, not
what worked.

---

## 9. Memory and prior session context

Auto-memory continues to work across sessions. Existing v3-era memories
about printf rules, no-stubs, BIOS-first, DuckStation oracle, etc. all
still apply. New v4-specific memories should be tagged so future
sessions can tell them apart from v3 memories. The most important new
memory is: **"v3 failed because it was an interpreter+HLE emulator
masquerading as a recompiler. v4 fixes this by ACTUALLY recompiling
the BIOS."**

---

## 10. No speculative progress

If a step involves:

- indirect jumps
- relocation
- hardware interaction

You MUST produce:

- manifest
- proof artifact

Code without proof is invalid.

---

## 11. First milestone is absolute

Before any Phase 2 work:

- FIRST_MILESTONE.md must be complete
- boot_slice must compile
- all instructions must be supported

No exceptions.

---

## 12. Relocation is mandatory before full BIOS

Do NOT attempt full BIOS recompilation until:

- BOOT_RELOCATION_PLAN.md is implemented
- address_aliases.json exists
- duplicate code is impossible

---

## 13. No large-step execution

You may NOT:

- "recompile the full BIOS"
- "walk the entire ROM"

Until:

- function discovery pipeline exists
- manifest output is verified

---

## 14. Unknown is acceptable. Guessing is not.

If something is unknown:

→ STOP  
→ produce artifact showing unknown  

Do NOT guess behavior.

---

## 15. Broken tooling is never acceptable. Fix it.

If a tool, command, or verification mechanism fails or returns
unexpected results:

→ **Fix the tool.** Diagnose why it failed and repair it.  
→ Do NOT route around it with indirect evidence.  
→ Do NOT infer correctness from two broken implementations agreeing.  

"The screenshot command returns black" is not a reason to skip visual
verification. It is a reason to fix the screenshot command.

"Both the native runtime and interpreter show the same wrong value"
does not make the value correct. It means both have the same bug.

If you cannot fix the tool, **ask the user** what they observe.
Never declare a result "correct" without direct verification against
the oracle.

---

## 16. DuckStation oracle setup (from fresh checkout)

On a fresh clone of this repo, the DuckStation oracle is not yet ready. Run:

```bash
git submodule update --init --recursive duckstation
bash tools/duckstation/setup.sh     # clones deps, applies psxrecomp_oracle.patch
bash tools/duckstation/build.sh     # builds Release x64 via MSBuild
```

The result is `duckstation/build/bin/duckstation-qt.exe` — the oracle binary.
For headless launch it needs `settings.ini` in `duckstation/build/bin/` with
`PathNTSCU = SCPH1001.BIN` under `[BIOS]` and `SearchDirectory = bios`, plus
`bios/SCPH1001.BIN` copied next to the exe. `tools/duckstation/setup.sh` handles
these post-build config steps. See `tools/duckstation/README.md` for the full
layout and how to regenerate the patch.

**Never edit the submodule source to work around upstream bugs.** If a change
is needed in DuckStation's side, edit in the working tree, then regenerate the
patch against the pinned base — so the change is reviewable in a single diff.

---

## 17. Phase 5 gate — fix hardware stubs before loading a game

`STUBS_TO_FIX.md` lists every known stub in the runtime. Before any
Phase 5 work (loading Tomba or any game EXE), every stub marked
"Phase 5+" in that file **must be implemented and oracle-verified**:

- **S3 — MDEC decoder** (FMV playback)
- **S4 — SPU audio synthesis** (sound output)
- **S5 — DMA channels 0/1/3/4** (MDEC, CDROM, SPU data pipes)

These cannot be tested until disc data flows, but they cannot be
skipped either. The first task of Phase 5 is to implement them, not
to load the game and see what breaks. Loading the game with known
stubs is how v3 ended up with 1808 lines of shims.
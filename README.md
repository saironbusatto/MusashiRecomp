## Current Milestone

The authoritative milestone document is [`FIRST_MILESTONE.md`](./FIRST_MILESTONE.md).
The authoritative phase plan is [`PLAN.md`](./PLAN.md), Phase 1 section.

- **Phase 1a — Boot Slice:** COMPLETE (2026-04-06). Reset-vector slice
  `0xBFC00000..0xBFC00074`, 30 instructions, terminating at
  `j 0xBFC00150`. `generated/boot_slice.c`,
  `generated/boot_slice_manifest.json`, and an empty
  `generated/unsupported_ops.json` are the proof artifacts. Do not
  redo. Do not extend.
- **Phase 1b — Instruction Coverage:** NEXT. Every R3000A opcode the
  BIOS uses must have a real implementation in `strict_translator`,
  with `generated/instruction_inventory.json` and
  `generated/instruction_coverage.json` as proof.
- **Phase 1c — Function Discovery Pipeline:** after 1b. Bounded walk
  from explicit seed file, producing `function_manifest.json`,
  `function_edges.json`, `discovery_run.log.json`.
- **Phase 1d — Indirect Control Flow Recording:** after 1c. Records,
  does not resolve. Produces `indirect_jumps.json` and the Ghidra
  cross-reference.
- **Phase 1e — Relocation Planning (HARD GATE):** after 1d. Produces
  `relocation_proofs/`, `address_aliases.json`,
  `normalization_rule.md`. Full BIOS recompilation is forbidden
  until 1e is complete.

See `PLAN.md` → "Phase 1 — Controlled Recompiler Bring-up" for
required artifacts, failure conditions, and acceptance criteria per
sub-phase.

---

## Non-Goals (Current Phase)

The following are explicitly out of scope for the entire Phase 1
window. Touching any of them is architectural drift and is not
permitted regardless of how convenient it would be:

- **No runtime.** There is no `runtime/` directory in v4 yet, and
  Phase 1 does not create one.
- **No MMIO.** No `psx_mmio_read_*` / `psx_mmio_write_*` entry
  points. `generated/cpu_state.h` stays compile-only.
- **No GPU.** No GP0, no GP1, no software rasterizer, no VRAM.
- **No interrupts.** No I_STAT, no I_MASK, no COP0 IRQ delivery
  hooks. (`mtc0`/`mfc0`/`rfe` are translated as instructions in
  Phase 1b — they are not wired to any runtime delivery path.)
- **No DMA, no timers, no SIO, no memcard, no CD-ROM, no SPU.**
- **No full BIOS recompilation.** Bounded walks only until Phase 1e
  closes.
- **No game loading.** No PS-X EXE ingestion. No ISO. No Tomba. The
  game side does not exist in v4 until Phase 5.
- **No BIOS interpreter.** Not as a fallback, not as a "temporary"
  bridge, not for "code we couldn't recompile yet".
- **No HLE BIOS functions.** No `bios.c`, no A0/B0/C0 reimplementation
  in C. The recompiled BIOS *is* the BIOS.

---

## Definition of Done (Current Phase)

Phase 1a (already met):

- `generated/boot_slice.c` compiles cleanly with `-c`.
- `generated/boot_slice_manifest.json` is complete.
- `generated/unsupported_ops.json` is empty.
- Every instruction in the slice is accounted for in the manifest.
- No TODO / FIXME / XXX in any generated file.

Phase 1 as a whole (the gate that unlocks Phase 2):

- All Phase 1a artifacts above continue to regenerate identically.
- `generated/instruction_inventory.json` and
  `generated/instruction_coverage.json` exist and show every
  BIOS-used opcode implemented in `strict_translator`.
- `generated/function_manifest.json`, `generated/function_edges.json`,
  and `generated/discovery_run.log.json` exist from a normalized
  Phase 1c re-run, with no duplicate functions under any normalized
  address.
- `generated/indirect_jumps.json`,
  `generated/indirect_jump_classes.json`, and
  `generated/indirect_jump_ghidra_xref.json` exist and cover every
  indirect jump site in every discovered function.
- `generated/relocation_proofs/` exists with one subdirectory per
  proven ROM→RAM copy operation, each containing both Ghidra and
  DuckStation evidence.
- `generated/address_aliases.json` and
  `generated/normalization_rule.md` exist and are mechanically
  derived from `relocation_proofs/`.
- `generated/unsupported_ops.json` is still empty.
- Nothing under `generated/` was hand-edited.



# PSXRecomp v4

A static MIPS-to-C recompiler for the PlayStation BIOS, with PS1 games as
a follow-on layer. The BIOS runs as **native compiled C**, not as
interpreted MIPS bytes.

This is a clean-slate restart after three failed prior attempts (v1, v2,
v3) all collapsed into emulator-with-HLE shape instead of true static
recompilation. See [`PLAN.md`](./PLAN.md) for the full background, the
architectural decision, the salvage list, the phase plan, and the open
questions we want outside review on.

## Layout

```
psxrecomp-v4/
├── PLAN.md              the document, hand to outside reviewers
├── CLAUDE.md            rules for in-session work (Architecture A locked in)
├── README.md            this file
├── bios/
│   └── SCPH1001.BIN     the recompilation target, 524288 bytes
├── duckstation/         modified DuckStation, used as runtime oracle
└── recompiler/          MIPS->C translator, salvaged from v3
    ├── src/
    ├── include/
    ├── lib/             rabbitizer, fmt, toml11
    └── CMakeLists.txt
```

The runtime is intentionally absent at this stage — it will be built up
fresh in Phase 2 once the recompiler can ingest the BIOS ROM and emit
`SCPH1001_full.c`.

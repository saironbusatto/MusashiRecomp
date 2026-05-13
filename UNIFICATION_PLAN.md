# UNIFICATION_PLAN — one recompiler, one runtime, TOML-driven

Start date: 2026-05-12 (post-validation-confusion session)

## Read first

Before any work in this plan, read `CURRENT_STATE.md` end-to-end. The
happy-path binary lives at `.happypath/tomba-runtime.exe`. Every step in
this plan ends with a checkpoint that confirms it still works.

## Target state

```
# ONE recompiler binary
psxrecomp bios/SCPH1001.toml             # produces generated/SCPH1001_*.c
psxrecomp games/tomba/game.toml          # produces tomba/generated/*.c
                                         # (implicitly also ensures BIOS regen is current)

# ONE runtime binary, parameterized at link time
psx-runtime                              # discless BIOS only — boots to memcard / CD player
psx-runtime tomba/game.toml              # with game — boots BIOS → Tomba → main menu
psx-runtime games/ff7/game.toml --disc 2 # multi-disc — swap to disc N
```

- **Recompiler**: one binary, reads a TOML, dispatches to BIOS-path or
  game-path based on the config's `[program] type = "bios" | "game"`.
- **Runtime**: one binary. Always includes BIOS recomp output. Conditionally
  includes game recomp output. If `[game]` is in the config, runtime calls
  out to game entry point after BIOS shell finishes its boot sequence.
- **Multi-disc**: `[discs]` array in game.toml; runtime exposes a disc-swap
  command on the debug port.

## Why we want this

Listed roughly in priority:

1. **Forces translator unification.** Today `strict_translator.cpp` (BIOS)
   and `code_generator.cpp` (game) are parallel implementations that have
   diverged. Bugs fixed in one don't apply to the other. Memory
   `strict_translator_stays_strict.md` directs that strict wins;
   code_generator should be absorbed into strict, not the reverse.
2. **Config-driven everything.** `bios/SCPH1001.toml` and
   `TombaRecomp/game.toml` already exist but are only used by audit tools.
   The recompiler should read them, not take positional `seeds.json`
   arguments.
3. **Always boots BIOS.** Today `psx-runtime` is the BIOS-only test runner
   and `tomba-runtime` is the BIOS+game runner — but `tomba-runtime` IS the
   discless BIOS path too (it boots BIOS first; you can in principle ignore
   the game). One runtime, two modes.
4. **Multi-disc** falls naturally out of a `[discs]` config array. Today's
   `[game] disc = "tomba.cue"` is a special case of a one-element array.
5. **Stop the two-process confusion.** Memory
   `architecture_two_processes_2026_05_05.md` notes we already have
   `psx-runtime` + `psx-beetle` (oracle) as parallel processes. Adding
   `tomba-runtime` as a third was a mistake we're cleaning up.

## What stays separate

- The Beetle oracle (`psx-beetle.exe`) stays its own process. It's an
  independent reference implementation; unifying it would defeat its
  purpose.

## Incremental phases

Each phase has a **checkpoint** that must pass before starting the next.
The checkpoint is always: `.happypath/tomba-runtime.exe` still boots and
reaches Tomba's main menu (or equivalent verification that the working
state has not been broken).

### Phase A — Config consolidation (low risk)

Goal: make both existing recompilers read their inputs from TOML, while
keeping `--emit-full` and friends working as backwards-compatible aliases.
No translator code touched.

A1. Define the unified TOML schema. Today there are two files:
- `bios/SCPH1001.toml` — has `[program]`, `[audit]`. Add `[recompiler]`
  with seeds path, output dir.
- `TombaRecomp/game.toml` — has `[game]`, `[recompiler]`, `[runtime]`,
  `[audit]`. Already richer.

  Unify into one schema with a top-level `type = "bios" | "game"` discriminator.

A2. Teach `psxrecomp-bios.exe` to accept `--config <path>.toml` as an
alternative to positional ROM+seeds args. Falling-through to existing
positional behaviour if no `--config`. Verify regen output is byte-for-byte
identical with both invocation styles.

A3. Teach `psxrecomp-game.exe` the same.

A4. Update CLAUDE.md, PLAN.md, and the helper cmake macro to use the
config-style invocation.

**Checkpoint A**: `.happypath/tomba-runtime.exe` still boots (untouched);
fresh rebuild of `tomba-runtime.exe` via the new TOML invocation produces
identical output to a fresh rebuild via the old positional invocation.

### Phase B — Runtime consolidation (medium risk)

Goal: collapse `psx-runtime` + `tomba-runtime` into one parameterized
runtime. After this phase, `psx-runtime` is gone as a separate target;
`tomba-runtime` is renamed to `psx-runtime` and takes an optional game
config.

B1. Identify the cmake macro `psxrecomp_v4_add_runtime_target` and audit
how `psx-runtime` (in this repo) and `tomba-runtime` (in TombaRecomp)
diverge. They probably mostly agree, but with paths and link inputs
parameterised differently.

B2. Define the merged target shape. Inputs:
- BIOS C source: always `generated/SCPH1001_*.c` (this repo).
- Game C source (optional): path from `[game] generated_dir`.
- Game config TOML: embedded as a runtime-readable resource (or path
  passed at runtime).

B3. Migrate `tomba-runtime` to be built via the merged macro, with
the game inputs supplied. **Do not touch the old `tomba-runtime.exe`
on disk yet** — build the new one alongside, verify it boots, then
swap.

B4. Migrate `psx-runtime` to be built via the merged macro, with no
game inputs. Confirm discless BIOS boot (or document the existing
psx-runtime-can't-boot-shell bug separately).

**Checkpoint B**: rebuilt-via-new-macro `tomba-runtime.exe` boots to
Tomba's main menu identically to `.happypath/tomba-runtime.exe`.

### Phase C — Recompiler translator unification (high risk)

This is the big one. Merge `strict_translator.cpp` + `code_generator.cpp`
into a single translator. Memory `strict_translator_stays_strict.md`
directs that the strict approach wins; `code_generator` content is
absorbed in.

C1. Inventory both translators' supported opcodes. For each, mark:
- supported on both sides (likely most)
- supported only in strict (BIOS-specific, e.g. CACHE, RFE handling)
- supported only in game (whatever Tomba uses that BIOS doesn't)
- handled differently between the two

C2. For divergences, decide which version wins:
- Hard rule: no fall-through cases sneak in (memory rule).
- Hard rule: no stubs (CLAUDE.md rule 0).
- For each divergence, document the resolution in
  `docs/translator_unification.md` so this audit is reviewable.

C3. Migrate the merged translator behind a feature flag. Build both
binaries (psxrecomp-bios and psxrecomp-game) against the new translator
in parallel with the old, run both regens, diff the output.

C4. When outputs match, flip the flag, delete the old translator.

C5. Rename `psxrecomp-bios.exe` and `psxrecomp-game.exe` → `psxrecomp.exe`.
Single binary, dispatches by config `type`.

**Checkpoint C**: same regen output, byte-for-byte, before and after the
flip. `.happypath/tomba-runtime.exe` checkpoint still passes when
relinked against the new regen output.

### Phase D — Multi-disc support

Goal: support `[discs] paths = [...]` in game.toml and runtime
disc-swapping. Not blocking on game work; can land any time after Phase B.

D1. Schema: `[game]` learns `discs = ["disc1.cue", "disc2.cue", ...]`.
For backwards compatibility, accept either `disc = "x.cue"` (single)
or `discs = [...]` (multi).

D2. Runtime exposes a `swap_disc <N>` debug command. CD-ROM subsystem
gates on which disc is currently mounted.

D3. Boot-time: mount disc 0 by default.

D4. Test on Tomba (single disc) to confirm no regression, then on a
real multi-disc game (FF7 / RE2 — TBD).

## Rollback / abort criteria

At any phase, if the checkpoint fails:

1. Restore `.happypath/tomba-runtime.exe` to its build location:
   ```
   cp .happypath/tomba-runtime.exe \
     F:/Projects/TombaRecomp/build/tomba-runtime.exe
   ```
2. Stop work, update `CURRENT_STATE.md` with what was learned, and
   surface the blocker to the user before retrying.

## Decisions made 2026-05-12

1. **BIOS config location**: extend `bios/SCPH1001.toml` in place. Add
   `[recompiler]` and `[runtime]` sections to the existing file. The
   `[program]` and `[audit]` blocks already there stay.
2. **Game configs**: stay in sibling game repos (e.g.
   `TombaRecomp/game.toml`). Matches existing sibling-junction +
   `.pin` pattern shared with nesrecomp / segagenesisrecomp.
3. **Runtime name**: keep `psx-runtime`. Becomes parameterized; the
   sibling-repo `tomba-runtime` target retires and the merged binary
   is invoked as `psx-runtime <game.toml>` (optional).
4. **Multi-disc test target**: deferred. Phase D designs the
   `[discs] = [...]` schema; we pick a concrete game (FF7 / RE2 / MGS)
   when we're actually ready to validate disc-swapping.

# PSXRecomp v4 — Overnight Session Hand-off (2026-04-06)

> **Audience:** a fresh Claude Code session that has been context-cleared
> and given this file as the canonical entry point. Matthew is asleep
> and will NOT be available to answer questions. You are running
> autonomously overnight.

---

## 1. READ FIRST (in this exact order, no skipping)

1. `CLAUDE.md` — the project constitution. Re-read it in full.
2. `PLAN.md` — Phase 1 sub-phase structure, hard gates, proof-driven rules.
3. `FIRST_MILESTONE.md` — Phase 1a frozen contract.
4. `README.md` — current milestone pointer.
5. `C:\Users\Matthew\.claude\projects\F--Projects-psxrecomp-v4\memory\MEMORY.md` — auto-memory index. Then read every linked memory file. The most important entries:
   - `strict_translator_stays_strict.md` — never add fallthrough cases
   - `phase_1a_complete.md` — boot slice baseline frozen 2026-04-06
   - `phase_1a_baseline_changed_for_alignment.md` — the baseline was DELIBERATELY updated during B(7); current baseline is post-B(7), not the original 2026-04-06 form
   - `srlv_isa_completeness_exception.md` — SRLV is the ONE allowed no-inventory-hit opcode. Don't generalize.
   - `rfe_substitutes_for_eret.md` — RFE handles the FIRST_MILESTONE.md "ERET" terminator.
   - `env_path_ordering.md` — `PATH=/c/msys64/mingw64/bin:$PATH` is REQUIRED for every cmake / gcc / psxrecomp-bios.exe invocation.
   - `matthew_communication_style.md` — terse, decisive, absolute no-stubs.

After reading, state out loud (in your first response):
- "Architecture A is locked. No interpreter. No HLE. No stubs. BIOS first. Game never until Phase 5."

If any of the above files cannot be read, **stop and write a status file `OVERNIGHT_STATUS.md` explaining the blocker.** Do not proceed.

---

## 2. CURRENT STATE (as of session end, 2026-04-06 ~22:30 PT)

### Phase 1a — COMPLETE
Boot slice `0xBFC00000..0xBFC00074`, 30 instructions, terminates at `j 0xBFC00150`. Frozen. Do NOT redo.

### Phase 1b — INSTRUCTION COVERAGE COMPLETE
The strict translator (`recompiler/src/strict_translator.cpp`) covers **54 inventory opcodes implemented + 2 implemented-but-unused (RFE, SRLV) + 2 deferred FPU (COP1, LWC1) = 58 total opcode classifications** out of the 56 inventory buckets. Every BIOS opcode in any walker-reachable region has a real audited implementation with the following exceptions:
- COP1 (17 hits) and LWC1 (2 hits): FPU instructions in the BIOS `atof` routine. PS1 R3000A has no FPU. Reachability unknown. Deferred until Phase 1c proves whether they're live.

#### Phase 1b sub-phase history (in order)
- **B(1)** — ALU R-type: ADD, ADDU, SUBU, AND, OR, XOR, NOR, SLT, SLTU. ADD overflow trap via `psx_arith_overflow` extern.
- **B(2)** — ALU I-type: ADDI, ANDI, XORI, SLTI, SLTIU. ADDI overflow trap via the same extern. SLTIU sign-extends the immediate first then unsigned-compares.
- **B(3)** — Shifts: SRL, SRA, SLLV, SRAV (and SLL was already in Phase 1a). **Plus SRLV via the ISA-completeness exception** — the ONLY opcode in the strict translator implemented without an inventory hit. Matthew explicitly approved this carve-out and it must NOT be generalized.
- **B(4)** — HI/LO + multiply/divide: MFHI, MFLO, MULTU, DIV, DIVU. DIV/DIVU divide-by-zero and INT32_MIN/-1 edge cases handled per documented R3000A behavior. **Remainder computed as `r = n - q*d`** (NOT `n % d`) to be host-`%`-independent — this was Matthew's correction during B(4).
- **B(5)** — Conditional branches: BEQ, BNE, BLEZ, BGTZ, BLTZ, BGEZ. **Pre-delay snapshot pattern via `TranslateResult.pre_delay_code`** — added because 441 of 4617 branches in the BIOS use the operand-mutating delay slot idiom. Snapshot variables are `psx_brA_<HEXADDR>` / `psx_brB_<HEXADDR>` declared at function scope before the delay slot, read in the branch terminator c_code. Verified by `tools/scan_branch_delay_hazards.py` which reports `incorrect_in_current_translator: 0`.
- **B(6)** — Loads: LB, LBU, LH, LHU, LW. Sign-extension via `(int32_t)(int8_t)` / `(int32_t)(int16_t)`. `$zero` writes use `(void)cpu->read_*(addr)` to preserve MMIO side effects. Alignment checks added in B(7) — see below.
- **B(7)** — Stores: SB, SH, SWL, SWR. **Also retroactively added alignment-fault checks to LH, LHU, LW, SH, SW** via `psx_unaligned_access` extern. **Phase 1a baseline byte-output for `boot_slice.c` was DELIBERATELY updated** to add the SW alignment guard. The new baseline is the canonical baseline going forward.
- **B(8)** — COP0 access: MFC0, MTC0. Direct `cpu->cop0[rd] <-> cpu->gpr[rt]` transfer with no SR-write hooks, no IRQ delivery, no exception modeling. `cop0[0]` is the Index register, NOT a hardwired zero — do not apply `$zero` rules to `cop0[]`.
- **B(9)** — Unaligned word loads: LWL, LWR. Little-endian R3000A semantics. Merge with old `rt` value via `psx_old_rt = cpu->gpr[rt]` snapshot. No alignment fault by design.
- **B(10)** — Traps: SYSCALL, BREAK. Both are **non-terminators** with `extern_call(...); return;` c_code. Decision forced by the walker: marking them as terminators would force the walker to fetch the next instruction as a "delay slot", and at e.g. `0xbfc0d8c4` the post-syscall instruction is `jr $ra` which is itself a terminator → walker would fail loud. SYSCALL passes `cpu->gpr[2]` (the PS1 ABI syscall number register), BREAK passes the 20-bit immediate from `(raw >> 6) & 0xFFFFF` plus the PC.

#### What still exists in `missing` / `deferred` / `impl-but-unused`
- `missing`: **0**
- `deferred`: COP1 (17 hits, FPU), LWC1 (2 hits, FPU). Both in `FUN_bfc02324` (an `atof` routine). Reachability TBD in Phase 1c.
- `impl-but-unused`: RFE (lives in copy-to-RAM exception handler not in walker scope), SRLV (ISA-completeness exception).

### Generated artifacts (all up-to-date)
All files in `generated/`:
- `boot_slice.c` — 30 instructions, post-B(7) form (with SW alignment guard)
- `boot_slice.o` — compiles cleanly with `gcc -c -Wall -Wextra`
- `boot_slice_manifest.json` — unchanged from Phase 1a
- `unsupported_ops.json` — empty `[]`
- `cpu_state.h` — 4 forward-declared externs: `psx_syscall`, `psx_arith_overflow`, `psx_unaligned_access`, `psx_break`
- `instruction_inventory.json` — 56 buckets, 44,629 instructions walked
- `instruction_coverage.json` — 54 implemented / 0 missing / 2 deferred / 2 impl-but-unused
- `branch_delay_hazards.json` — 4617 branches scanned, 441 have operand-mutating delay slots (informational — handled correctly via snapshot), `incorrect_in_current_translator: 0`
- `ghidra_function_starts.json` — all 666 Ghidra function entry points + 9 verified function bodies

### Tools (all in `tools/`)
- `build_instruction_inventory.py` — BFS reachability walker over the BIOS, deduped, classified, regenerates `instruction_inventory.json`. Verified against 9 Ghidra-sampled function-end checks. **NEVER MODIFY THE WALKER LOGIC** without explicit instruction.
- `build_instruction_coverage.py` — joins inventory against the strict translator's hand-maintained "implemented" table, regenerates `instruction_coverage.json`.
- `scan_branch_delay_hazards.py` — proves the snapshot fix is in place. Reports `incorrect_in_current_translator: 0`. Reads `recompiler/src/strict_translator.cpp` source to verify each branch handler uses the snapshot pattern. **If this scanner ever reports nonzero, STOP and write OVERNIGHT_STATUS.md.**
- `_dump_inventory.py`, `_dump_suspect.py`, `_dump_unknown.py`, `_demo_branch_emit.py` — small dev helpers, leave alone.

### Recompiler binary
`recompiler/build/psxrecomp-bios.exe` — built. Re-run with:
```bash
PATH=/c/msys64/mingw64/bin:$PATH ./recompiler/build/psxrecomp-bios.exe bios/SCPH1001.BIN generated
```

### Documentation
- `PLAN.md` — Phase 1 fully expanded with sub-phases. Phase 1c is the next major milestone.
- `README.md` — current milestone pointer is up-to-date.
- `FIRST_MILESTONE.md` — frozen Phase 1a contract.

---

## 3. WHAT YOU MAY DO OVERNIGHT

These are tasks that are **safe to perform autonomously** because they're either:
- Pure analysis (no source changes)
- Self-validating (regression baselines exist)
- Strictly within Phase 1b's scope, with the exit gate already met

### Priority 0 — verify the handoff (do this FIRST, every time you boot)
Run, in order:

```bash
# 1. Verify Ghidra MCP is reachable (per CLAUDE.md session-start checks)
# Use mcp__ghidra__get_program_info — should return SCPH1001.BIN @ bfc00000..bfc7ffff, 666 functions

# 2. Rebuild psxrecomp-bios from a forced-clean .obj
cd F:/Projects/psxrecomp-v4/recompiler
rm -f build/CMakeFiles/psxrecomp-bios.dir/src/strict_translator.cpp.obj \
      build/CMakeFiles/psxrecomp-bios.dir/src/bios_slice_walker.cpp.obj
PATH=/c/msys64/mingw64/bin:$PATH /c/msys64/mingw64/bin/cmake.exe --build build --target psxrecomp-bios

# 3. Snapshot baseline, regenerate, diff
cd F:/Projects/psxrecomp-v4
cp generated/boot_slice.c /tmp/bs_pre.c
cp generated/boot_slice_manifest.json /tmp/bsm_pre.json
cp generated/unsupported_ops.json /tmp/uo_pre.json
PATH=/c/msys64/mingw64/bin:$PATH ./recompiler/build/psxrecomp-bios.exe bios/SCPH1001.BIN generated
diff -q /tmp/bs_pre.c generated/boot_slice.c
diff -q /tmp/bsm_pre.json generated/boot_slice_manifest.json
diff -q /tmp/uo_pre.json generated/unsupported_ops.json
# All three diffs MUST be silent (no output). If any prints a difference, STOP.

# 4. Compile-check
PATH=/c/msys64/mingw64/bin:$PATH /c/msys64/mingw64/bin/gcc.exe -c -Wall -Wextra -I generated generated/boot_slice.c -o generated/boot_slice.o
# Exit must be 0, no warnings.

# 5. Regenerate inventory + coverage + hazard scan
PATH=/c/msys64/mingw64/bin:$PATH python tools/build_instruction_inventory.py
PATH=/c/msys64/mingw64/bin:$PATH python tools/build_instruction_coverage.py
PATH=/c/msys64/mingw64/bin:$PATH python tools/scan_branch_delay_hazards.py
# Expected:
#   inventory: 56 buckets, 44629 instructions, 0 unknown
#   coverage:  54 implemented, 0 missing, 2 deferred, 2 impl-but-unused
#   hazard:    incorrect_in_current_translator: 0
# Any deviation = STOP, write OVERNIGHT_STATUS.md.
```

### Priority 1 — strict-translator audit pass (ANALYSIS ONLY, no edits)
Read `recompiler/src/strict_translator.cpp` end-to-end. For each opcode case, verify:
- `r.supported = true;` is set on the success path
- The c_code matches the documented MIPS-I R3000A semantics
- `$zero` handling is correct (either via `emit_gpr_write` or via an explicit `if (rt == 0)` branch that still does any side effects)
- Any signed-int operation that could overflow has a guard or uses unsigned arithmetic
- Variable shift counts use `& 0x1Fu` masks
- Branch targets use `simm * 4` not `simm << 2` (to avoid signed-shift UB)
- Branches use the snapshot pattern (`psx_brA_` / `psx_brB_` only, no `cpu->gpr[` in the c_code)
- Loads/stores compute `addr = (uint32_t)((int32_t)cpu->gpr[base] + (simm))`
- Aligned ops (LH/LHU/LW/SH/SW) check `addr & MASK` before accessing memory and call `psx_unaligned_access(cpu, psx_addr, 0xPCu); return;` on misalignment

Write findings to a NEW file `generated/strict_translator_audit_2026-04-06.md` (do NOT overwrite anything). If you find a real bug, STOP and write `OVERNIGHT_STATUS.md` explaining it. Do NOT fix the bug — fixes need Matthew's approval.

### Priority 2 — proof artifact: spot-check a sampled BREAK and SYSCALL
Use `mcp__ghidra__get_code` (format: `disassembly`) on the function containing the first BREAK example (`0xbfc012cc`) and the first SYSCALL example (`0xbfc0d8c4`). Confirm the surrounding code matches the patterns the strict translator expects:
- BREAK should follow a divide instruction or a conditional check (compiler-inserted trap).
- SYSCALL should be in a thin wrapper function with `addiu $a0/$a1/$a2/...; syscall; jr $ra; nop` pattern.

Write findings to `generated/break_syscall_spot_check_2026-04-06.md`. No source edits.

### Priority 3 — additional sampled function-end verification
The current `ghidra_function_starts.json` has 9 verified function bodies. Pick ~10 more random functions across the BIOS (not adjacent to the existing 9) and call `mcp__ghidra__get_function_info` on each to capture their bodies. Add them to the `verified_function_bodies` array in `ghidra_function_starts.json`. Re-run `tools/build_instruction_inventory.py` — if any new sample fails verification, STOP. If all pass, you've strengthened the regression baseline.

**Constraint:** when adding to the verified list, preserve the existing 9 entries exactly. Append new ones.

### Priority 4 — write `PHASE_1B_COMPLETE.md`
Create a NEW file `PHASE_1B_COMPLETE.md` (do NOT overwrite anything else) summarizing:
- What Phase 1b delivered (54 ops, with the table from this hand-off's section 2)
- What Phase 1a invariants are still preserved (boot slice byte-identical, etc.)
- What is deferred to Phase 1c (FPU reachability, RFE coverage via copy-to-RAM walk)
- What is deferred to Phase 2 (the four trap externs: psx_syscall, psx_arith_overflow, psx_unaligned_access, psx_break)
- The pre_delay_code contract for branches (with the example from `_demo_branch_emit.py`)
- The SRLV ISA-completeness exception
- The Phase 1a baseline change for alignment guards

Match the tone of `FIRST_MILESTONE.md` — terse, declarative, no fluff. Around 200-300 lines.

### Priority 5 — Phase 1c PREP (planning only, no code)
Phase 1c is "function discovery pipeline" per `PLAN.md`. The CURRENT state has:
- 666 Ghidra function entry points dumped
- A BFS walker that visits each function's reachable instructions
- A coverage report

What Phase 1c needs that DOESN'T YET EXIST:
- A C++-side function-discovery walker integrated into the recompiler (not just the Python analysis tool)
- An explicit seed file `recompiler/seeds/phase1c_seeds.json`
- Per-function manifest output `function_manifest.json` with edges, termination reasons, lineage
- Indirect-jump recording for Phase 1d
- Deduplication via normalized addresses (Phase 1e relocation work)

Write a Phase 1c TODO list to `generated/phase1c_prep_2026-04-06.md`. **Do NOT begin implementing Phase 1c.** This is planning only.

---

## 4. WHAT YOU MUST NOT DO OVERNIGHT

These are HARD prohibitions. Violating any of these = stop immediately and write `OVERNIGHT_STATUS.md`.

### Source code
- ❌ Do NOT modify `recompiler/src/strict_translator.cpp`. Phase 1b is closed.
- ❌ Do NOT modify `recompiler/src/strict_translator.h`. The TranslateResult contract is frozen.
- ❌ Do NOT modify `recompiler/src/bios_slice_walker.cpp` or `.h`. The walker is frozen.
- ❌ Do NOT modify `recompiler/src/code_generator.cpp` (the salvaged v3 code). It is NOT trusted.
- ❌ Do NOT modify `generated/boot_slice.c` or `generated/cpu_state.h`. Build artifacts and hand-maintained header.
- ❌ Do NOT add new opcodes to the strict translator, even ones that look "obviously needed".
- ❌ Do NOT remove SRLV (the ISA-completeness exception) or RFE (the unused-but-implemented terminator).
- ❌ Do NOT add MULT, MTHI, MTLO, BLTZAL, or BGEZAL — Matthew explicitly rejected these as speculative.
- ❌ Do NOT touch the Python BFS walker logic. Adding NEW analysis scripts is fine; modifying the existing inventory walker is not.
- ❌ Do NOT modify any test/build infrastructure (CMakeLists.txt, etc.).

### Architectural moves
- ❌ Do NOT begin Phase 1c implementation. Planning is OK; coding is not.
- ❌ Do NOT begin Phase 1d (indirect jump recording).
- ❌ Do NOT begin Phase 1e (relocation planning).
- ❌ Do NOT create a runtime/ directory.
- ❌ Do NOT add MMIO, GPU, interrupts, DMA, timers, SIO, memcard, CD-ROM, SPU, or any peripheral simulation.
- ❌ Do NOT load a game ISO, PS-X EXE, or anything game-related.
- ❌ Do NOT walk the full BIOS (the BFS walker over Ghidra functions is OK because it's analysis; a recompiler-side full walk is not).
- ❌ Do NOT write any C runtime that defines `psx_syscall`, `psx_break`, `psx_arith_overflow`, or `psx_unaligned_access`. Their forward-declared / link-fail-by-design contract is intentional.
- ❌ Do NOT write a MIPS interpreter for ANY reason.
- ❌ Do NOT add any HLE BIOS shim.
- ❌ Do NOT touch DuckStation.
- ❌ Do NOT push to a remote, do not commit (Matthew controls git).

### Process
- ❌ Do NOT use `fprintf`, log files, or any printf debugging. The TCP debug server doesn't exist yet; structured JSON artifacts are the only output channel.
- ❌ Do NOT speculate. If something is unknown, write it as `unknown` in an artifact and move on. The phrases "probably", "should work", "looks correct", "seems like" are forbidden in any artifact you write.
- ❌ Do NOT create files with `// TODO`, `// FIXME`, `// XXX`, or `// for now`. The post-emission scanner will reject them and Matthew will be unhappy.
- ❌ Do NOT bulk-search-and-replace anything.
- ❌ Do NOT delete files.
- ❌ Do NOT modify memory files in `C:\Users\Matthew\.claude\projects\F--Projects-psxrecomp-v4\memory\` unless you're appending a new note about something Matthew explicitly told you to remember (he didn't, so don't).

---

## 5. WHEN TO STOP AND WRITE `OVERNIGHT_STATUS.md`

Stop and write a status file at the project root if ANY of these happen:

- Any Priority-0 sanity check fails (rebuild fails, diff shows changes to baseline, compile errors, hazard scanner reports nonzero, inventory or coverage counts deviate from the expected values).
- You discover a real bug in the strict translator during the Priority-1 audit. Document the bug; do NOT fix it.
- Ghidra MCP is unreachable.
- A required tool fails (cmake, gcc, python).
- You're tempted to do something that's on the prohibited list.
- You're unsure whether something is allowed. **Default to "stopped" and write the status file.**
- You've completed all 5 priorities and have nothing else safe to do. (Write a "completed all overnight tasks" status.)

The status file format:
```markdown
# OVERNIGHT_STATUS.md
Generated: <ISO timestamp>
Session uptime: <approximate>

## Summary
<one-line summary of why you stopped>

## What I completed
- <list of finished priorities and any artifacts created>

## What I did NOT complete
- <list of skipped priorities and why>

## Findings (if any)
<anything Matthew needs to know in the morning>

## Sanity-check results
- rebuild: pass/fail
- baseline diff: clean/dirty
- compile: pass/fail
- inventory: <numbers>
- coverage: <numbers>
- hazard scan: <numbers>

## Next steps for Matthew
<what you recommend he look at first when he wakes up>
```

Place it at `F:/Projects/psxrecomp-v4/OVERNIGHT_STATUS.md`.

---

## 6. ENVIRONMENT CHEAT SHEET

### Path requirement
**Every** invocation of cmake, gcc, or psxrecomp-bios.exe MUST be prefixed with:
```
PATH=/c/msys64/mingw64/bin:$PATH
```
The user's default PATH is broken for the mingw64 toolchain. Ignoring this will cause "command not found" errors that look like the build is broken when it isn't.

### Working directory
```
F:/Projects/psxrecomp-v4
```
Use forward slashes in paths even on Windows. Bash is the shell.

### Bash/Windows quirks
- Use `/dev/null`, NOT `NUL`
- Use `cp` and `diff -q`, NOT Windows equivalents
- The `Bash` tool runs in MSYS2 bash on Windows; it understands forward slashes
- Files at `C:\Users\Matthew\...` are accessible via Windows-style paths in `Read`/`Write` tool calls

### Ghidra MCP
The Ghidra MCP server is auto-started. Tools you may need:
- `mcp__ghidra__get_program_info` — sanity check
- `mcp__ghidra__get_function_info` — verify function bodies
- `mcp__ghidra__get_code` — disassembly per function
- `mcp__ghidra__list_functions` — paginated list (use offset/limit)
- `mcp__ghidra__get_hexdump` — raw bytes at an address

Active program: `SCPH1001.BIN`, base `0xBFC00000..0xBFC7FFFF`, 666 functions.

### Tools you should NOT use overnight
- `Edit`, `Write` — only for new artifact files in `generated/` or new audit/planning markdown files. NEVER for source files in `recompiler/src/`, `recompiler/include/`, `bios/`, or existing files in `generated/` other than the analysis JSONs.
- `Bash` with destructive flags — no `rm -rf`, no `git push`, no `git reset --hard`.
- `Agent` (subagent dispatch) — keep work in your own context. Subagents have their own rate-limit pool and will exhaust it quickly on MCP-heavy work. Spawn one ONLY if you need to do >100 sequential MCP calls and would otherwise blow your own context.

---

## 7. KNOWN PHASE 2 / PHASE 1c HAZARDS (read but do nothing about)

These are issues that WILL need to be addressed in later phases. Knowing about them helps you understand why the current state is what it is.

### Reset-vector blind spot
Ghidra's first analyzed function is at `0xbfc00420`. The reset vector and pre-function-1 region `0xBFC00000..0xBFC0041F` is covered by the synthetic seed walker but Ghidra does not classify it as a function. Phase 1c needs to seed `0xBFC00000` explicitly. The current Python BFS walker already does this; the C++-side recompiler walker (which doesn't exist yet) will need to as well.

### `0xBFC00100` is NOT a valid seed
The PLAN.md correction documents this: PS1's R3000A has no TLB, so the R3000 UTLB-miss vector is unused. The bytes there are part of the "Sony Computer Entertainment Inc." copyright string. If you see `0xBFC00100` anywhere as a "seed" or "exception vector", it's wrong.

### `0xBFC00180` IS a valid seed
The BEV=1 General Exception vector. 168 instructions reachable from there.

### RFE lives in copy-to-RAM bytes
The exception handler is at `0x80000080` in RAM, not in the ROM-side function set. The strict translator implements RFE but the inventory walker never reaches it (hence "impl-but-unused"). Phase 1e relocation work will add the copy-to-RAM walk.

### COP1 / LWC1 (FPU) reachability unknown
17 + 2 instances in `FUN_bfc02324` (an `atof`-style routine). PS1 has no FPU; these would trap in real hardware. Whether the BIOS actually executes them depends on whether anything in the reachable code calls `FUN_bfc02324`. Phase 1c reachability analysis will answer this.

### 441 branches with operand-mutating delay slots
Handled correctly via the pre_delay_code snapshot pattern. The scanner verifies. Don't worry about it unless the scanner ever reports `incorrect_in_current_translator > 0`.

### Four forward-declared trap externs
- `psx_syscall(cpu, code)` — Phase 1a
- `psx_arith_overflow(cpu)` — Phase 1b B(1) ADD/ADDI overflow
- `psx_unaligned_access(cpu, addr, pc)` — Phase 1b B(7) misalignment
- `psx_break(cpu, code, pc)` — Phase 1b B(10) BREAK trap

ALL four are declared in `generated/cpu_state.h` and never defined anywhere. Linking the recompiled BIOS into a runnable binary will fail until the Phase 2 runtime provides definitions. This is intentional and required.

---

## 8. FINAL PRE-FLIGHT CHECK

Before you do ANYTHING, write your first response containing exactly these sections in this order:

1. **Files read.** List every file you read from section 1 above. If any failed, stop.
2. **Architecture statement.** Verbatim: "Architecture A is locked. No interpreter. No HLE. No stubs. BIOS first. Game never until Phase 5."
3. **Sanity check results.** Run all 5 Priority-0 commands and report results.
4. **Plan.** Which of priorities 1-5 you intend to work on, in order.
5. **Stop conditions.** Restate verbatim that you will write `OVERNIGHT_STATUS.md` and stop if any of the conditions in section 5 are met.

Do NOT skip the pre-flight check. Do NOT condense it. Matthew will read it in the morning.

---

## 9. ONE LAST THING

Matthew has been reviewing and approving every batch tonight in real time. The pace and the precision of his corrections (DIV remainder formula, branch snapshot pattern, alignment checks, SRLV exception, MULT comment) show he cares about the small details and trusts you to get them right. Don't squander that trust by inventing changes.

If you have nothing useful to do that fits within the rules in section 4, **write a status file saying so and stop.** Sleeping is better than drift.

Good luck. See you in the morning.

— Claude (B(10) session, 2026-04-06 ~22:30 PT)

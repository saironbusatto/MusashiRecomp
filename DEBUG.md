# DEBUG LOOP (EXECUTION CONTRACT)

Follow EXACTLY. No deviation.

---

# RULE 0 — TOOL VALIDATION (FIRST USE)

On the FIRST use of ANY tool:

You MUST validate what it is doing and its output

Required:
1. Cross-check against another source
2. Verify structure AND content
3. Confirm assumptions

Never:
- Trust output blindly
- Assume correctness

If not validated:
-> ALL reasoning is INVALID

---

# LOOP

0. Tool validation (first use only)
1. Sync state (NOT frame number)
2. Dump full state (native + oracle) via TCP debug server
3. Diff bytes
4. Find FIRST divergence
5. Trace writer (function + instruction + call path)
6. Classify (codegen / runner / timing / config)
7. Fix the tool (never generated output)

If ANY step is skipped:
-> STOP and restart

All inspection must use TCP (Phase 2+).
Before TCP exists (Phase 1), use structured JSON artifacts only.

---

# PSX-SPECIFIC INSPECTION TARGETS

When comparing native vs DuckStation oracle:

- CPU registers (32 GPRs, HI, LO, PC, COP0)
- RAM contents at known kernel addresses (PCB, TCB, EvCB tables)
- VRAM contents (byte-for-byte)
- MMIO read/write sequences
- Timer counter values
- Interrupt fire ordering
- DMA transfer sequences
- GPU command streams

---

# CLASSIFICATION

codegen:
- Wrong C emitted by strict_translator
- Fix: recompiler/src/strict_translator.cpp

runner:
- Wrong hardware simulation (MMIO, DMA, IRQ, GPU, timers)
- Fix: runtime/src/*.c

timing:
- Events fire at wrong cycle / wrong order
- Fix: runtime timing logic

config:
- Missing function seed, wrong address alias
- Fix: seed files, address_aliases.json

discovery:
- Function not found, dispatch miss
- Fix: function finder pipeline

NEVER fix generated output. Fix the tool that produced it.

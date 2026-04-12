# PSXRecomp v4 — Phase 3 HANDOFF (FRESH SESSION)

You have ZERO prior context. Follow this exactly.

---

## 0. FIRST ACTION

Read these files in order:

1. CLAUDE.md
2. PLAN.md (Phase 3 starts ~line 1047)
3. PRINCIPLES.md
4. DEBUG.md
5. MEMORY.md

Then state EXACTLY:

"Architecture A is locked. No interpreter. No HLE. No stubs. BIOS first. Game never until Phase 5."

If you cannot confidently state this, STOP.

---

## 1. CURRENT STATE (DO NOT REDO)

Phase 1 COMPLETE — recompiler proven correct on SCPH1001.BIN
Phase 2 COMPLETE — runtime executes recompiled BIOS
Phase 3 IN PROGRESS — BIOS boots into shell loop, no display yet

### What works RIGHT NOW

The recompiled BIOS executes from reset through kernel init, copies
kernel Part 2 to RAM, sets up A0/B0/C0 function tables, processes a
SYSCALL, copies the shell from ROM to RAM 0x80030000, enters the shell,
and runs in a stable loop for 3+ seconds without crashing.

**97,000+ trampoline resolutions per 3-second run.**
**~800 seeds, ~793 emitted functions, 46,499 instructions.**

### Trusted baseline files

Generated (DO NOT MODIFY — fix the recompiler instead):
- `generated/SCPH1001_full.c` — 793 functions, 46,499 instructions
- `generated/SCPH1001_dispatch.c` — binary-search dispatch with 795 entries

Runtime:
- `runtime/src/main.cpp` — loads BIOS, initializes CPU, calls dispatch
- `runtime/src/memory.c` — memory + MMIO routing (SEE STUB AUDIT BELOW)
- `runtime/src/traps.c` — SYSCALL handler + trampoline resolver
- `runtime/include/cpu_state.h` — CPU state definition

Recompiler (changes made this session):
- `recompiler/src/full_function_emitter.cpp` — fallthrough codegen, J/JAL relocation, shell normalization
- `recompiler/src/function_discovery.cpp` — normalization range fix
- `recompiler/src/strict_translator.cpp` — MTHI/MTLO added
- `recompiler/seeds/phase2_ghidra_seeds.json` — 800 seeds

### Architecture invariants (STILL TRUE)

- Functions: `void func_XXXXXXXX(CPUState*)`
- All memory via function pointers (read/write)
- Address normalization: `addr & 0x1FFFFFFF` + relocation rules
- Two relocation zones:
  - Kernel Part 2: ROM `0x1FC10000-0x1FC17FFF` ↔ RAM `0x500-0x8500`
  - Shell: ROM `0x1FC18000-0x1FC427FF` ↔ RAM `0x30000-0x5A7FF`
- Dispatch: `psx_dispatch(cpu, addr)` with binary search
- JAL → direct call; JR $ra → return; JALR/indirect JR → `psx_dispatch`
- Runtime trampoline resolver handles A0/B0/C0 vectors in RAM
- SYSCALL simulates R3000A exception (EPC, Cause, SR push, vector dispatch)

---

## 2. STUB AUDIT — WHAT IS REAL VS FAKE

This is the critical section. Read `runtime/src/memory.c` carefully.

### REAL hardware simulation (correct behavior):
- Memory control registers (0x1F801000-0x1F801020): store/recall
- RAM size register (0x1F801060): store/recall
- Cache control (0xFFFE0130): store/recall
- Interrupt I_STAT (0x1F801070): AND-acknowledge semantics ✓
- Interrupt I_MASK (0x1F801074): store with 0x7FF mask ✓
- SPU registers (0x1F801C00-0x1F801FFF): store/recall (no audio needed)
- Expansion 1 (0x1F000000-0x1F7FFFFF): returns 0xFFFFFFFF (open bus) ✓
- Expansion 2 / POST (0x1F802000-0x1F802FFF): ignored ✓

### STUBS that MUST be replaced with real simulation:

**GPU (0x1F801810, 0x1F801814) — CRITICAL for Sony logo:**
- GP0 write: silently dropped (line 150)
- GP1 write: silently dropped (line 151)
- GPUREAD: returns 0 (line 102)
- GPUSTAT: returns fixed 0x1C000000 (line 103)
- REALITY: needs full GP0 command parser, FIFO, VRAM writes, display params

**DMA (0x1F801080-0x1F8010FF) — CRITICAL for GPU data transfer:**
- All reads return 0 (line 90-92)
- All writes silently dropped (line 138-140)
- REALITY: needs per-channel MADR/BCR/CHCR, DPCR/DICR, transfer execution
- GPU DMA (channel 2) transfers polygon/texture data from RAM to GPU

**Timers (0x1F801100-0x1F80112F) — NEEDED for vblank:**
- All reads return 0 (line 93-96)
- All writes silently dropped (line 141-143)
- REALITY: needs counter values, mode registers, target values, overflow flags
- Timer 1 (hblank) drives the vblank interrupt

### STUBS that are ACCEPTABLE for Phase 3:
- SIO (0x1F801040-0x1F80105F): returns TX ready — OK until Phase 4
- CDROM (0x1F801800-0x1F801803): returns 0 — OK until Phase 5
- MDEC (0x1F801820-0x1F801824): returns 0 — not needed for Sony logo

### unmapped_fatal is a concern:
- Currently returns silently instead of aborting (line 62-68)
- This was necessary for BIOS RAM-size probing during init
- BUT it could mask real bugs during shell execution
- Consider: re-enable fatal abort for writes, keep silent for reads

---

## 3. PHASE 3 REMAINING GOAL

Render the Sony Computer Entertainment logo.

REQUIREMENT: VRAM output must match DuckStation byte-for-byte.

---

## 4. WHAT MUST BE BUILT NEXT (in order)

### Step 1: GPU command processor
The shell is writing GP0/GP1 commands that are being silently dropped.
Implement:
- GP1 command parser (display mode, display area, drawing area, reset)
- GP0 command parser (fill rect, copy rect, draw polygon, draw sprite)
- 1MB VRAM buffer (1024×512 × 16-bit)
- GPUSTAT register with real status bits

### Step 2: DMA channel 2 (GPU)
The shell uses DMA to transfer large command lists to the GPU.
Implement:
- Per-channel registers (MADR, BCR, CHCR) for all 7 channels
- DPCR (channel enable/priority)
- DICR (DMA interrupt control)
- Channel 2 (GPU) linked-list and block transfer modes
- Channel 6 (OTC — ordering table clear)

### Step 3: Timer + vblank interrupt
The shell waits for vblank to sync frame display.
Implement:
- Timer 0/1/2 counter registers
- Timer mode registers (clock source, target, IRQ)
- Vblank interrupt delivery (I_STAT bit 0)
- Simple frame timing (call vblank at ~60Hz equivalent)

### Step 4: SDL2 display window
- Add SDL2 dependency to CMakeLists.txt
- Create window (640×480 or 320×240)
- Blit VRAM framebuffer region to SDL surface each frame
- Main loop: execute BIOS → check vblank → blit → SDL_PollEvent

---

## 5. SALVAGE SOURCE (v3 reference, NOT copy-paste)

v3 hardware sim files at:
`F:/Projects/psxrecomp-projects-v3/psxrecomp/runner/src/`

- `gpu.c` (1200+ lines) — GP0/GP1 command parser, FIFO, GPUSTAT
- `gpu_sw_renderer.c` — software rasterizer (polygon, sprite, fill)
- `dma.c` (378 lines) — DMA channel transfer engine
- `interrupts.c` (325 lines) — I_STAT/I_MASK + exception dispatch
- `timers.c` (150+ lines) — counter advance, mode flags, clock sources

AUDIT RULES (from CLAUDE.md §7):
- REMOVE: all bios_*, interpreter_*, events/threading, HLE logic, kernel mirrors
- KEEP ONLY: pure hardware register simulation, state machines
- Integrate ONE file at a time → wire into memory.c → test immediately

---

## 6. BIOS ROM MEMORY MAP (verified)

Code regions (scan for functions HERE ONLY):
- `BFC00000-BFC0DC60` — Kernel Part 1 (~56KB)
- `BFC10000-BFC16760` — Kernel Part 2 (~26KB, relocated to RAM 0x500)
- `BFC18000-BFC42800` — Shell (~172KB, copied to RAM 0x80030000)

Data regions (NEVER scan, NEVER treat as code):
- `BFC0DC60-BFC0FFFF` — zero padding
- `BFC16760-BFC17FFF` — data tables + zero padding
- `BFC42800-BFC63FFF` — graphics (TIM textures, strings, CLUTs)
- `BFC64000-BFC7FFFF` — font bitmaps (112KB)

---

## 7. BUILD

```
PATH=/c/msys64/mingw64/bin:$PATH
export TMP=/c/msys64/tmp TEMP=/c/msys64/tmp

# Recompiler (only if recompiler source changes):
cd recompiler/build && cmake --build .

# Re-emit (only if seeds or recompiler changes):
cd ../.. && recompiler/build/psxrecomp-bios.exe bios/SCPH1001.BIN generated \
  --emit-full recompiler/seeds/phase2_ghidra_seeds.json

# Runtime:
cd runtime/build && cmake --build .

# Run:
cd ../.. && runtime/build/psx-runtime.exe bios/SCPH1001.BIN
```

Current behavior: runs silently in a loop (shell executing with stubbed hardware).
Expected after GPU impl: should render Sony logo to VRAM, display via SDL2.

---

## 8. DEBUGGING METHOD

DuckStation TCP debug server:
- Location: `F:/Projects/psxrecomp-v4/duckstation/build/bin/`
- Launch: `duckstation-qt.exe -bios`
- Port: 4371
- Commands: ping, read_ram, get_registers, gpu_state, pause, step

For EVERY GPU command or DMA transfer you implement, verify against
DuckStation's output for the same boot sequence.

---

## 9. HARD CONSTRAINTS (still in force)

NEVER:
- Return dummy MMIO values without understanding what the BIOS expects
- Skip hardware behavior  
- Use printf debugging (no fprintf to stderr/stdout in runtime code)
- Modify generated code
- Add HLE behavior
- Load a game ISO or EXE

If an MMIO read value is unknown:
1. Check DuckStation (TCP debug server)
2. Check Ghidra for the calling BIOS code's usage pattern
3. Implement the correct producing logic

---

## 10. KNOWN ISSUES

1. `unmapped_fatal` is silent — could mask bugs. Consider re-enabling for writes.

2. GPUSTAT returns fixed `0x1C000000` — the shell reads this to check GPU
   readiness. When GPU is implemented, this must reflect real FIFO/busy state.

3. SIO_STAT returns fixed `0x00000005` — adequate for Phase 3 but must be
   real for Phase 4 (controller/memcard).

4. The trampoline resolver in `traps.c` handles runtime-written code at
   A0/B0/C0 vectors. It simulates delay slots for `li+jr` patterns.
   This is NOT an interpreter — it's a dispatch chain resolver. But verify
   it handles all patterns the shell uses.

5. One BIOS function skipped (COP1/FPU at BFC02324) — not reached during boot.

---

## 11. SUCCESS CRITERIA

Phase 3 is DONE when:
- SDL2 window opens
- Sony Computer Entertainment logo renders on screen
- VRAM contents match DuckStation byte-for-byte

Phase 3 is NOT done if:
- "The GPU command stream looks right" (PIXELS ON SCREEN)
- "VRAM has some data" (MATCHES DUCKSTATION)
- "It mostly works" (BYTE FOR BYTE)

---

BEGIN.

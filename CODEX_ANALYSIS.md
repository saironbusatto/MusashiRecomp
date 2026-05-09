# Codex Analysis - PSXRecomp v4 Memory Card Copy

Date: 2026-05-07
Branch: `codex/discovery`

## Executive Summary

We now have a real, non-no-op reproduction for memory-card COPY.

The current Card 2 setup before this session was a poor proof target: Card 2 already contained the same first three saves as Card 1, so a Card1 -> Card2 copy could visually "succeed" while the destination file hash stayed unchanged. I replaced Card 2 with a formatted blank card for both native and Beetle, then repeated the same COPY flow.

Result:

- Beetle oracle copies successfully.
- Native PSXRecomp does not copy.
- The old `func_1FC0A080.$a1 = 0x83` blocker is not the current blocker in this repro.
- Native enters `func_1FC0A080` with a valid pointer, but the downstream SIO write protocol aborts before the write payload is sent.

Current localized failure:

Native starts slot-1 memory-card write commands (`0x57`) but each aborts after only five bytes:

```text
81 57 00 00 00
```

That is: card select, write command, ID/address phase, then interruption before the 128-byte data payload. Because the state machine never reaches `MC_WRITE_CHK`, `memcard_write_sector()` is never called, so `card2.mcd` remains blank.

The next target is native vs Beetle comparison around the first slot-1 `0x57` write transaction, with focus on SIO IRQ scheduling / pad polling / transaction isolation.

## Current Disk State

Working tree has intentional card-image changes:

```text
M card2.mcd
M dummy.1.mcr
```

Current card hashes:

```text
card1.mcd   15DBADBB080B8561414DB62847594E4C
card2.mcd   8A250FE6206A1537011166866B509EBD  # blank, native did not copy
dummy.0.mcr 97C780723BDA08D8880CFB33421F9FCC
dummy.1.mcr 2CB83FE744EFCD085B9EA8C17D3EBF31  # Beetle copied one save
```

Backup of the pre-test Card 2 images:

```text
F:\tmp\psxrecomp_card_backup_20260507_182441
```

Current hidden processes at time of writing:

```text
psx-runtime.exe pid 39100, debug port 4370
psx-beetle.exe  pid 10560, debug port 4380
```

Only one instance of each backend was running.

## Method Of Navigation

I followed the local repo rules and the `F:\Projects\recomp-template` debugging approach:

- Treat Beetle as oracle and native as subject.
- Avoid generated-file edits.
- Avoid HLE pokes, stubs, log files, and direct state forcing.
- Use TCP debug commands, ring buffers, screenshots, and card hashes.
- Establish a proof target before inferring behavior.
- Prefer first-divergence evidence over static guessing.

Concrete navigation steps:

1. Preserved the current dirty state by committing it before investigation.
2. Created and stayed on `codex/discovery`.
3. Verified both hidden debug servers:
   - native: `127.0.0.1:4370`
   - Beetle: `127.0.0.1:4380`
4. Verified Ghidra MCP connectivity separately:
   - `ghidra_psx` works through Codex at `http://localhost:7777/mcp`.
   - Active binary is `SCPH1001.BIN`.
   - Ghidra confirmed the static A080 chain from the earlier handoff.
5. Validated that native `fntrace` was not the right tool for static generated function entries.
   - Generated function entries are captured by `fn_filter` / `fn_clear` / `fn_entry_dump`.
   - `fntrace_arm` is more useful for indirect dispatch / Beetle JAL/JALR comparison.
6. Used screenshots after every navigation step where input ambiguity mattered.
7. Switched from duplicate Card 2 to blank Card 2 to make copy success observable by hash and directory contents.

## Reproduction Setup

Build command used earlier in the session:

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
cmake --build runtime\build --target psx-runtime psx-beetle -- -j8
```

Start exactly one native and one Beetle:

```powershell
Start-Process -FilePath "F:\Projects\psxrecomp-v4\runtime\build\psx-runtime.exe" -WorkingDirectory "F:\Projects\psxrecomp-v4" -WindowStyle Hidden
Start-Process -FilePath "F:\Projects\psxrecomp-v4\runtime\build\psx-beetle.exe"  -WorkingDirectory "F:\Projects\psxrecomp-v4" -WindowStyle Hidden
```

Blank Card 2 setup:

```powershell
Copy-Item runtime\build\card1.mcd card2.mcd -Force
Copy-Item runtime\build\card1.mcd dummy.1.mcr -Force
```

`runtime\build\card1.mcd` and `runtime\build\card2.mcd` are formatted blank cards with no used directory entries.

Native uses:

```text
card1.mcd
card2.mcd
```

Beetle uses:

```text
dummy.0.mcr
dummy.1.mcr
```

## UI Repro Flow

Starting from BIOS main menu with cursor on `MEMORY CARD`:

1. CROSS: enter memory-card manager.
2. DOWN: select `COPY`.
3. CROSS: enter source-save selection.
4. CROSS: choose the first Card 1 save.
5. Confirm modal appears: `COPY FILE / Are you sure?`
6. CROSS on `YES`.

Important: screenshots are needed after steps 2-5. I initially misread the cursor and accidentally pressed CROSS on `EXIT`, which returned to main menu. The reliable proof requires seeing the cursor on `COPY`, then seeing the file/source icon selected, then seeing the modal.

Useful commands:

```powershell
python tools\raw_tcp.py 4370 screenshot_file path=F:\tmp\native_step.bmp
python tools\raw_tcp.py 4380 screenshot_file path=F:\tmp\beetle_step.png
```

Native screenshot output currently ignores the requested path and writes `psx_screenshot.bmp` in the repo root. Beetle honors the requested path.

## Key Native Evidence

Before confirming YES, arm native function-entry capture for A080:

```powershell
python tools\raw_tcp.py 4370 fn_filter lo=0x1FC0A080 hi=0x1FC0A081
python tools\raw_tcp.py 4370 fn_clear
python tools\raw_tcp.py 4370 card_data_writes_reset
```

After confirming YES:

```powershell
python tools\raw_tcp.py 4370 fn_stats
python tools\raw_tcp.py 4370 fn_entry_dump addr_lo=0x1FC0A080 addr_hi=0x1FC0A081 count=20
python tools\raw_tcp.py 4370 card_txn_dump count=400 tail=1
python tools\raw_tcp.py 4370 card_data_writes count=16
```

Native A080 entries showed:

```text
func = 0x1FC0A080
ra   = 0x00002D14
a0   = 0x000086CC
a1   = 0x8007A1D0
a2   = 0x00000080
a3   = 0x00000003
t1   = 0xBFC0A080
```

So the previous `a1 = 0x83` A080 path is not reproduced here. The current path passes a valid RAM pointer.

Native transaction summary around the failed write:

```text
cmd counts in recent window:
0x52: 390
0x57: 10

0x57 transactions:
slot=1
cmd=0x57
sector=0xFFFF
bytes=5
end_reason=abort_other
terminal_state=5
tx = 81 57 00 00 00
rx = FF 00 5A 5D 00
```

The state naming from `runtime/src/sio.c`:

```text
MC_IDLE      = 0
MC_CMD       = 1
MC_ID1       = 2
MC_ID2       = 3
MC_ADDR_MSB  = 4
MC_ADDR_LSB  = 5
MC_WRITE_DATA starts after address phase
```

Because native stops at terminal state 5, it never enters `MC_WRITE_DATA` for the payload and never reaches `MC_WRITE_CHK`.

Native SIO trace around the first failed write:

```text
seq 128916 tx=81 rx=FF mc_pre=0 mc_post=1 ctrl=0x3013 func=0x1FC42370 in_exc=0
seq 128917 tx=57 rx=00 mc_pre=1 mc_post=2 ctrl=0x3013 func=0x1FC42370 in_exc=0
seq 128918 tx=00 rx=5A mc_pre=2 mc_post=3 ctrl=0x3013 func=0x1FC42370 in_exc=0
seq 128919 tx=00 rx=5D mc_pre=3 mc_post=4 ctrl=0x3013 func=0x1FC42370 in_exc=0
seq 128920 tx=00 rx=00 mc_pre=4 mc_post=5 ctrl=0x3013 func=0x1FC42370 in_exc=0

then pad polling interrupts:

seq 128921 tx=01 rx=FF mc_pre=0 mc_post=0 ctrl=0x1013 func=0x0000445C in_exc=1
seq 128922 tx=42 rx=41 ...
seq 128923 tx=00 rx=5A ...
seq 128924 tx=00 rx=FF ...
seq 128925 tx=00 rx=BF ...
```

The transaction ring reports the `0x57` transaction closed as `abort_other` at terminal state 5. This is likely because the card flow is interrupted/reset before the next byte after `MC_ADDR_LSB`.

Native card status after the failed copy:

```text
slot1 dirty=false
card2.mcd hash remains 8A250FE6206A1537011166866B509EBD
card2.mcd has no used directory entries
```

## Key Beetle Evidence

Beetle with blank `dummy.1.mcr` successfully copies the first save.

Before:

```text
dummy.1.mcr MD5 = 8A250FE6206A1537011166866B509EBD
```

After:

```text
dummy.1.mcr MD5 = 2CB83FE744EFCD085B9EA8C17D3EBF31
directory slot 1 = BISLPS-01234SAVE00
```

Beetle A080 trace:

```text
target = 0xBFC0A080
kind   = JALR
caller = 0x00002D0C
ra     = 0x00002CDC
a0     = 0x000086CC
a1     = 0x8007A458
```

Beetle hit A080 repeatedly during the copy. The pointer differs from native, but both are valid RAM pointers. The important divergence is not "native never gets to A080"; it does. The divergence is downstream in native SIO write completion.

## Current Hypothesis

Native is allowing pad/VBlank polling or interrupt handling to interleave with an in-progress card write at a point where Beetle keeps the write transaction progressing.

The write command starts correctly:

```text
81 57 00 00 00
```

Then native switches to pad polling before sending the 128-byte write payload. This causes the card transaction to close at address phase and the BIOS retries the short write several times. No commit occurs.

Candidate areas:

- SIO IRQ delay/countdown behavior after card ACKs.
- Whether `sio_card_protocol_active()` considers `MC_ADDR_LSB` / write address phase active strongly enough to defer pad polling.
- VBlank/pad poll re-entry while card write is mid-transaction.
- CTRL/select handling around slot changes (`0x3013` card slot 1 vs `0x1013` pad slot 0).
- Transaction close classification may be diagnostic only, but it exposes the real symptom.

Do not patch by forcing writes or faking card state. The correct fix should preserve hardware behavior and let the BIOS send the full write transaction.

## Recommended Next Steps

1. Capture Beetle SIO/write transaction shape for the successful copy, especially around its first slot-1 `0x57`.
2. Add or use equivalent Beetle SIO byte trace if needed; current Beetle fntrace proves A080 but not the full SIO byte stream.
3. In native, reset rings immediately before confirming YES and capture:
   - `sio_trace`
   - `sio_pc_trace` filtered to `0x1F801040` and `0x1F80104A`
   - `sio_irq_dump src=card` if useful
   - `imask_trace`
   - `card_txn_dump`
4. Find the first point where native leaves the card write path after `MC_ADDR_LSB`.
5. Fix the runtime SIO/IRQ scheduling behavior, not BIOS state.

Likely code locations:

```text
runtime/src/sio.c
runtime/include/sio.h
runtime/src/debug_server.c      # instrumentation only
runtime/src/beetle_debug_server.c / beetle_libretro.cpp if oracle trace parity is needed
```

## Guardrails

Keep following the project rules:

- No generated file edits.
- No HLE-poking card/directory state.
- No stubs.
- No `fprintf` logging or external log files.
- No game ISO/EXE.
- Do not run multiple native or multiple Beetle instances simultaneously.
- Do not seed `0xBFC0989C`.
- Treat `0xBFC0A080.$a1 = 0x83` as an older/stale path unless it is reproduced again with this blank-card proof target.

## Update 2026-05-08 - COPY Write Path Fixed

The SIO/IRQ hypothesis above was superseded by the next trace pass.

Root cause:

- The write callback did enter `func_000051F4`, but its state jump table at RAM `0x6C70` was not emitted as a C `switch`.
- The table contains low kernel-RAM aliases such as `0x0000542C`, which maps to ROM label `0xBFC14F2C`.
- `full_function_emitter.cpp` and `function_discovery.cpp` only mapped shell RAM aliases (`0x30000..0x5AFFF`) back to ROM when resolving jump tables. They did not map kernel-part-2 RAM aliases (`0x500..0x84FF`) back to `BFC10000+`.
- Because the emitter missed that table, generated `func_000051F4` did `cpu->pc = $t7; return` for state `0x542C`. `psx_dispatch` then fell into `dirty_ram_dispatch`.
- The dirty RAM interpreter executes one basic block. It handled the `jal 0x6380` in the write-byte block, but it could not resume at the generated post-call continuation `BFC14F80`, where `$v0` is cleared to `0`.
- `func_4D6C` saw stale `$v0 = 0x7564`, treated that as an unexpected callback result, reset SIO, and aborted every write at five bytes.

Fix:

- Added low kernel RAM alias resolution in both:
  - `recompiler/src/function_discovery.cpp`
  - `recompiler/src/full_function_emitter.cpp`
- Regenerated `generated/SCPH1001_full.c` and `generated/SCPH1001_dispatch.c`.
- The regenerated `func_000051F4` now emits:

```text
jump table at 0x00006C70, 10 entries
case 0x0000542C: goto label_BFC14F2C;
```

Proof after rebuild:

```text
Before YES:
card2.mcd MD5 = 8A250FE6206A1537011166866B509EBD

After YES:
card2.mcd MD5 = 30B8B147D550A409E1C5EF8456B8F18A
Card 2 shows the copied save icon in the BIOS memory-card UI.
```

Transaction proof:

```text
new slot-1 write transactions: 64
reasons: {'success': 64}
lengths: {138: 64}
sectors: 0x0040..0x007F
```

Probe proof:

```text
BFC14F78 pre-6380 marker:  20
BFC14F79 post-6380 marker: 20
BFC14F80 clear-v0 block:    20
BFC14934 reset path:        0
```

This is a codegen/discovery fix, not a runtime SIO workaround.

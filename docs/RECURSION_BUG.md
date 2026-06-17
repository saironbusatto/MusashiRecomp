# Tomba long-run recursion freeze — working doc

Branch: `bug/recursion`. This is the single source of truth for the long-run
freeze investigation and the soak-harness regression that is currently blocking
reproduction. Update it as we learn.

---

## 1. The bug we are actually chasing (the freeze)

**Symptom.** Tomba runs correctly for a long time, then hard-freezes. Intermittent;
*often sooner than 15 minutes* but **not reliable**. The captured specimen is
`_freeze_specimens/tomba_4393_freeze_46440.dmp` (frame 46440).

**Root cause (from the minidump).** A **runaway guest recursion that never unwinds.**
The per-frame game-flow state machine re-enters *itself* across the
**compiled ↔ dirty-RAM-interpreter boundary**. Validated, symbolized cycle:

```
exec_one (main loop 0x8001A51C)
  -> psx_dispatch_game_compiled(0x8001A954)
    -> 0x8001A954 -> 0x8001AC00 -> 0x8001B2B4 -> 0x8001B5A8 -> 0x80046264
      -> dirty_ram_dispatch
        -> psx_dispatch_game_compiled(0x8001A954)   <-- back to the top
```

Repeats ~1443 times -> native **fiber stack overflows** -> stack-depth guard halts
the process (`psx_fatal_halt`, which parks or exits). The state-machine fields
`[+0x4a]/[+0x4c]/[+0x4e]` on the controller object at `0x801FD800` are pinned at
`1/1/1` and never advance — that is the cycle that won't terminate. The ~1443
frames overflow nearly instantly *once the cycle starts*; the "~15m / intermittent"
is how long until the **trigger condition** is met, not how long the recursion runs.

**What it is NOT** (all prior hypotheses, refuted):
- NOT data corruption — `0x8001a954` is not a wild pointer; it exists nowhere in
  RAM as data.
- NOT nested interrupt delivery — `in_exception=0`, exception counts normal.
- NOT a fiber deadlock/livelock.
- Scene id intact, table `DAT_8007c640` intact.

**Why it's slippery to capture.** `g_psx_dispatch_depth` reads only 5 — it is blind
to this, because the 1443 frames are direct `func_X(cpu)` calls it does not count.
The `recent_fn` ring is time-ordered, so it shows only the leaf churning at the trip,
**not** the recursing cycle. **This is the entire reason the native_stack tool
exists** (see §3).

**Suspected trigger.** CD-DMA dirty-page over-marking: `dma.c:695` marks every
DMA'd word executable/dirty and never clears it (463/512 RAM pages dirty at the
freeze), time-correlated with the onset of boundary re-entry.

**Fix territory (NOT yet done).** The interp<->compiled call/return contract
(`dirty_ram_dispatch` / `psx_dispatch_game_compiled`) must **unwind** at the
boundary instead of **nesting**; and/or stop `dma.c:695` marking non-code CD-DMA
data as executable/dirty. Same family as the "wild call contract" recursion bugs
(Bug A/C/D). The fix is in the **recompiler**, never in generated C.

---

## 2. The reproduction harness ("soaks") — and the regression blocking it

**Workflow.** Run **4 instances** in parallel ("Soak A/B/C/D"). Claude launches
them; the user navigates **2 to a New Game** and **2 into Dwarf Forest** (overworld),
then lets all four **idle**. Running 4x in parallel is how we hit the intermittent
freeze fast. Per-instance memcard dirs (`saves_a..d`), **software renderer**, 4:3,
ports 4393-4396 (`tomba_soak_{a,b,c,d}.toml`). Stay off heavy host work while
soaking (a wall-clock starvation watchdog, 4s no-heartbeat -> exit(2), false-trips
under host load).

**The regression (currently blocking everything).** On the current
uncommitted/debug build, **Soaks A-D crash the instant anything loads to the
overworld** — before idle, before any chance to reproduce the freeze. SIGSEGV,
**no `psx_last_run_report.json`**. This did not happen on the build that used to
soak faithfully for ~15m.

---

## 3. The native_stack tool (the thing that may be causing the regression)

Uncommitted tooling added to `runtime/src/crash_trace.c` (129 lines): a
host-stack walker `append_native_stack()` that recovers the true recursion cycle
(the `recent_fn` ring can't — see §1). It is called **only** from
`psx_crash_trace_dump()`, i.e. only while a crash/fatal/SEH dump is already in
progress. It walks from the faulting/the current `rsp` up to the fiber `StackBase`,
keeps only validated return addresses (value in `.text` preceded by a `call`),
run-length-collapses them, and emits module-relative RVAs + a histogram. Every
stack read is bounded by `VirtualQuery`; symbolize offline with `nm`
(`_freeze_specimens/analyze_named.py`, `decode_report.py`).

**Known hazard (handoff issue #3).** Both `psx_signal_handler` and
`psx_seh_handler` run **on the faulting stack** and call the dump. If the original
fault is the recursion's stack overflow, `append_native_stack` runs on the
already-exhausted stack and can **re-fault** — taking the whole report with it
(silent SIGSEGV, no JSON). As written, the tool meant to capture the overflow can
**destroy the very report it was built for.**

---

## 4. Evidence collected this session (artifact-level)

Compared `TombaRecomp/build-soak` (A, has tooling, crashes) vs
`TombaRecomp/build-soak-e` (B, pure master, healthy). MD5'd all 49 objects in each:
**only two differ** — `crash_trace.c.obj` and `main.cpp.obj`. `main.cpp` is an
AppImage `getenv("APPIMAGE")` change, **inert on Windows**. All generated BIOS/game
code (`SCUS_942.36_*.c.obj`, `SCPH1001_*.c.obj`) is **byte-identical MD5**. The
psxrecomp reflog shows HEAD parked at master `3ba40b0` since 09:34 that day, so A
and B were regen'd off the **same** master tip — **not** a flavor trap, **not**
stale generated code. (Those were the handoff's two hypotheses; both refuted.)

**Open logical tension.** `crash_trace.c`'s only new code runs inside the dump
path, and **every dump caller terminates**. So if a dump fired on overworld-load,
build B (no tooling) would stop too — contradicting "B gets into game." Two ways
out, to be settled empirically (§5):
1. B was never actually driven through the exact "New Game -> overworld" path the
   soaks use; OR
2. **the overworld-load regression is in `master` itself** (this morning's merges:
   ws-backdrop-coverage @09:27, controller change @09:34), not the tooling. The A-vs-B
   diff can't see a master regression because both A and B are post-09:34 master.

---

## 5. Plan

1. **(done)** Branch `bug/recursion`, write this down.
2. **Unravel the overworld-load regression — decisive A/B from source.** Build two
   clean builds from current master: **X = master + native_stack tooling +
   "flush report before the walk" hardening**; **Y = pure master, no tooling**
   (the existing `build-soak-e` already IS Y). Run 2 soaks on X + 2 on Y, navigate
   all to overworld:
   - Y also crashes -> it's a **master regression**; bisect master against the
     last-known-good faithful-soak commit.
   - Only X crashes -> it's the **tooling**; X's report (now flushed before the
     walk) shows the real trigger — recursion-fired-immediately vs a separate fault.
   - X crashes with **no** report even after the early flush -> the fault is
     **outside** `psx_crash_trace_dump` -> not `append_native_stack`; look elsewhere.
3. **Return to the soak workflow** on whichever build survives overworld-load,
   reproduce the freeze, and capture it with native_stack (the early-flush makes the
   report survive the overflow walk).
4. **Fix the recompiler** (interp<->compiled unwind contract and/or `dma.c:695`
   over-marking). Regen, rebuild, re-soak to confirm. No stubs, no HLE, never edit
   generated C.

---

## 6. Build & run (this project)

```
cd /f/Projects/psxrecomp/TombaRecomp
export PATH=/c/msys64/mingw64/bin:$PATH TMP=/c/msys64/tmp TEMP=/c/msys64/tmp
# regen tools + BIOS + game (generated/ is gitignored; off master tip):
cmake --build ../psxrecomp/recompiler/build --target psxrecomp-game psxrecomp-bios -j8
(cd ../psxrecomp && ./recompiler/build/psxrecomp-bios.exe --config bios/SCPH1001.toml)
../psxrecomp/recompiler/build/psxrecomp-game.exe --config tomba_soak_e.toml
# build:
cmake -S . -B <build-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=ON -DPSX_LAUNCHER=OFF \
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe
cmake --build <build-dir> --target psx-runtime -j8
# run (per-instance card; taskkill first):
taskkill //F //IM psx-runtime.exe 2>/dev/null
nohup ./<build-dir>/psx-runtime.exe --game tomba_soak_a.toml > _soak_a.log 2>&1 &
# decode a freeze report's native_stack:
nm -n <build-dir>/psx-runtime.exe | awk '$2~/^[Tt]$/{print $1,$3}' > ../_freeze_specimens/allsyms.txt
python ../_freeze_specimens/decode_report.py psx_last_run_report.json ../_freeze_specimens/allsyms.txt
```

**Hard rules:** fix the recompiler, never generated C; no printf/log files (TCP
debug server + always-on rings; read `psx_last_run_report.json` for fatals);
per-instance memcard dir per concurrent instance (sharing corrupts the card ->
fake "regression"); software renderer for soaks; `taskkill //F //IM psx-runtime.exe`
before any launch.

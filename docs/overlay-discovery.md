# Overlay Discovery

**Date:** 2026-05-29  
**Branch:** `overlay-discovery`  
**Phase gate:** Phase 5+ (game EXE loaded, dirty-RAM hits observed in game address space)

---

## Problem

PSX games load code from disc at runtime into RAM. The main EXE on disc does not contain these
overlays — they arrive via CD-ROM DMA during gameplay. The static recompiler sees only the main
EXE at build time; it cannot know "at address 0x80090000, after area 2 loads, there will be
function X" without actually running the game.

The dirty-RAM interpreter handles these hits today, but it is slow and has no coverage guarantee.
This doc describes a self-improving two-layer system that eventually eliminates all runtime
interpretation of overlay code.

---

## Distinction from BIOS dirty-RAM

BIOS dirty-RAM (Rule 18 in CLAUDE.md) is code the BIOS *assembles in-place* at boot — e.g. the
4-instruction SIO handler written to RAM 0xCF0. That code never existed on disc.

Game overlays are different: they are complete compiled MIPS modules on the disc filesystem,
loaded to fixed RAM addresses. Their bytes are known ahead of time; the recompiler just hasn't
seen them yet. This system fixes that.

Missed heuristics (static-EXE functions the recompiler failed to identify) are orthogonal: they
exist in the main EXE at build time and will not match any disc file hash. The overlay system
ignores them and they fall through to dirty-RAM as today. Fix missed heuristics by improving
heuristics; fix overlays by this system.

---

## Architecture

Two layers, composing. As Layer B grows, Layer A fires less. Eventually Layer A goes silent.

### Layer B — Persistent map → static precompilation

**Runtime side:** whenever CD DMA completes a load into RAM, record `(disc_file_hash, load_addr,
size)` to a per-game log file on disk. This is cheap — a hash computation and a file append.

**Build-time side:** `recompiler/recomp.py` reads the log file. For each entry, it locates the
matching file on the disc image by hash, disassembles it at the recorded load address, and
emits a compiled C function set alongside the main EXE output. The overlay's functions are added
to the static dispatch table at their load addresses.

**Result:** any overlay logged in a previous run is statically compiled into the next build. No
runtime overhead on subsequent loads.

### Layer A — Runtime background compilation with persistent cache

**On CD DMA completion:**
1. Hash the written bytes.
2. Check the on-disk compiled cache (`cache/<SCUS_ID>/<hash>.dll`).
   - Cache hit → `LoadLibrary` the cached DLL, register its functions in the live dispatch table
     for `[load_addr, load_addr + size)`. No interpretation ever needed.
   - Cache miss → fall through to dirty-RAM interpretation. Queue a background thread to compile
     this overlay (emit C for the bytes at `load_addr`, invoke the C compiler, write the DLL to
     cache). On the *next* load of this overlay (next room visit, next level reload), the cache
     hit path fires instead.

**Result:** within a single playthrough, each overlay compiles once on first encounter. Every
subsequent encounter in that session, and in all future sessions, hits the cache.

### How they compose

```
Dispatch path (on branch to overlay address):
  1. Static dispatch table (Layer B) → precompiled function    [zero runtime cost]
  2. Compiled cache (Layer A)        → LoadLibrary'd function  [one-time per session after first]
  3. Dirty-RAM interpreter            → fallback               [first-ever encounter only]
```

Layer B coverage grows with every rebuild. Layer A covers the gap within a session before the
rebuild happens. Dirty-RAM is the last-resort fallback that fires exactly once per undiscovered
overlay.

---

## Game identification

Cache and log files are namespaced by SCUS/SLUS/SCES product code, read from `SYSTEM.CNF` on the
disc image at startup (`BOOT=cdrom:\SCUS_941.60;1` → key `SCUS-94160`).

```
cache/
  SCUS-94160/          ← Tomba (NTSC-U)
    <hash>.dll
    <hash>.dll
  SCES-01234/          ← different region, separate cache
    ...

logs/
  SCUS-94160/
    overlay_map.jsonl  ← append-only: {hash, load_addr, size, disc_file}
```

Different regional versions get separate caches automatically, which is correct since overlay
load addresses may differ between regions.

---

## Cache invalidation

A cached DLL is keyed by the hash of the overlay bytes. The hash IS the identity. If a disc file
changes (different dump, different region), the hash changes and the old cache entry is simply
never matched — it sits unused. No explicit invalidation needed.

When Layer B statically compiles an overlay, Layer A's cached DLL for that hash becomes
unreachable (static dispatch wins at step 1). It can be pruned post-build but is benign if left.

---

## Integration point

The natural hook is the CD-ROM DMA completion callback in `runtime/src/cdrom.c`. This is where
bytes land in RAM. After the DMA write:
1. Compute hash of the destination range.
2. Log `(hash, load_addr, size, source_file)` for Layer B.
3. Check Layer A cache. If hit, register the DLL's functions in the live dispatch table.
4. If miss, mark the range dirty (existing dirty-RAM path) and enqueue background compilation.

---

## Implementation phases

| Phase | Work |
|---|---|
| B-1 | CD DMA completion hook logs `(hash, load_addr, size)` to `logs/<SCUS_ID>/overlay_map.jsonl` |
| B-2 | `recomp.py` reads the log and emits overlay functions alongside the main EXE; adds entries to dispatch table |
| B-3 | Build-time validation: for each log entry, verify disc file hash matches before compiling |
| A-1 | On DMA completion, check `cache/<SCUS_ID>/<hash>.dll`; if present, `LoadLibrary` + register |
| A-2 | Background thread: emit C for newly-seen overlay, compile to DLL, write to cache |
| A-3 | On next load of same overlay, Layer A cache hit fires instead of dirty-RAM |

B-1 through B-3 can ship before A-1. The system is useful with only Layer B (just requires a
rebuild after each new playthrough). Layer A is the quality-of-life layer that eliminates the
"rebuild required" friction.

---

## Open questions

- Does Tomba use named disc files (ISO 9660) or raw LBA sector reads for overlay loads? Named
  files make Layer B trivial (hash the file, record the name). Raw LBA reads require tracing the
  sector number through to the ISO to recover the file identity. Determine by inspecting
  dirty-RAM dispatch miss addresses: if they cluster at 2-3 fixed load addresses, it's classic
  file-based overlays. If they scatter, it may be more ad-hoc.

- `LoadLibrary` / `dlopen` infrastructure does not currently exist in the runtime. Layer A
  requires it. Layer B does not. Implement B first; defer A until B has real coverage data.

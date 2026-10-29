# Overlay Discovery

**Updated:** 2026-05-30
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

Game overlays are different: they are compiled MIPS modules loaded to fixed RAM addresses. Their
bytes are known after they land in RAM; the recompiler just hasn't seen them yet. This system
fixes that.

Missed heuristics (static-EXE functions the recompiler failed to identify) are orthogonal: they
exist in the main EXE at build time. Fix missed heuristics by improving heuristics; fix overlays
by this system.

---

## Why disc extraction is not used

The naive approach — scan the disc image for overlay bytes by CRC — does not work for Tomba and
many other PSX titles. Investigation confirmed that Tomba stores overlays in a **scatter-load
format**: each disc sector is a `(12-byte block descriptor, 2048-byte data block)` pair, where
the descriptor tells the loader which RAM address to write the data block to. The resulting RAM
content is not present anywhere on disc as a contiguous byte range, so no CRC or brute-force scan
can find it.

The universal correct primitive is to capture bytes **at DMA time** — intercepting them as they
land in RAM, after all scatter-reassembly and decompression. This works for:

- Scatter-load formats (Tomba, others)
- Game-specific LZ or RLE compression (bytes arrive decompressed via DMA)
- Direct file loads (trivially captured)

### Exception: a per-game disc decoder (Brave Fencer Musashi)

When a game's container format is understood, every overlay can be decoded
straight from the disc, which covers areas nobody has played yet.
`tools/musashi_disc_overlays.py` does this for BFM:

- Code lives in the `MAIN.CD` / `SC01–07.CD` archives (`u32 count, u32 0`, then
  `(first_sector, byte_size)` pairs). Each entry is a run of `PAC` chunks, one
  per sector-aligned header: `"PAC\0", u8 type, u8 more, u16, u32 param,
  u32 size`. Type 1 is a raw copy and type 4 is the EXE's LZSS (`FUN_80018730`:
  1 KiB ring, cursor starting at 1, LSB-first flags).
- The load address is a runtime argument, not part of the file. The tool
  recovers it from the image's own absolute references: `jal` targets,
  word-sized pointers, and their sum each vote for the bases that land them
  on in-image prologues. A base is accepted only when a clear winner exists
  (≥16 votes, 1.5× the runner-up) and every clear winner agrees. If the base
  is wrong, the code is never executed: the loader runs a compiled function
  only while live RAM matches the bytes it was compiled from.
- Seeds come from `jal` targets and callable pointers in the EXE, in the image
  itself and in modules loaded at *other* slots. Modules that share a slot
  replace each other, so their references never count for one another. Two
  more sources are added: stack-frame prologues right after a `jr ra`, and the
  word after a resident module's header word.

Result on the US disc: 159 code images at 6 slots (136 scene builds at
0x80128158, plus 0x800CAE08, 0x800CCB1C, 0x800CEDF8, 0x801A00D8 and
0x801E4C60). These hold ~95k unique functions. `compile_overlays.py --static`
links them into the binary (see `runtime/include/overlay_static.h`). The 138
images that have no provable base are mostly data or tiny actor modules, and
they stay on the dirty-RAM interpreter.

```bash
python3 tools/musashi_disc_overlays.py --disc "<BFM>.cue" --exe SLUS_007.26 \
    --out build/musashi_disc_overlays.json
# parallel shards share --out-dir: pass --no-finalize, then finalize once
python3 tools/compile_overlays.py --static --no-finalize --cps --force \
    --captures build/musashi_disc_overlays.json --game-toml games/musashi/game.toml \
    --recompiler recompiler/build/psxrecomp-game --runtime-include runtime/include \
    --out-dir games/musashi/generated/overlays_static
python3 tools/compile_overlays.py --static-finalize \
    --out-dir games/musashi/generated/overlays_static
```

---

## Architecture

Two layers, composing. As Layer B grows, Layer A fires less. Eventually Layer A goes silent.
Layer B is a manifest of Layer A's output: the same `(load_addr, bytes, seeds)` data, committed
to the repo and baked into the binary ahead of time.

### Layer B — Build-time pre-compilation

**Runtime side (capture):** on every CD DMA completion into game-code RAM, the runtime appends
`(load_addr, size, bytes)` to a ring buffer. On process exit it writes
`overlay_captures.json` automatically — no user action required. The user discovers the file
exists and can attach it to a GitHub issue.

**Build-time side:** `psxrecomp-game` reads `game.toml [[overlays]]` entries, each pointing to
a captured `.bin` file and a list of seed entry points. It disassembles the MIPS bytes at
`load_addr`, emits C functions, and adds them to the static dispatch table. The shipped binary
has those overlays pre-compiled at zero runtime cost.

**Coverage grows** with every release as developer playthroughs and user contributions feed new
captures into the build.

### Layer A — Runtime background compilation with persistent cache

**On CD DMA completion into game-code RAM:**

1. Hash the written bytes.
2. Check `cache/<SCUS_ID>/<hash>.dll`.
   - **Cache hit** → `LoadLibrary` the DLL, register its functions in the live dispatch table
     for `[load_addr, load_addr + size)`. Interpreter never fires for this overlay.
   - **Cache miss** → fall through to dirty-RAM interpretation. Enqueue a background thread to
     compile this overlay: emit C for the bytes at `load_addr`, invoke the C compiler, write the
     DLL to cache. On the *next* load of this overlay the cache hit fires.

**Result:** within a single session, each new overlay is interpreted once and compiled once.
Every subsequent encounter — same session or future sessions — dispatches to native code.

A user who plays through the entire game accumulates a complete cache. From that point forward
the game runs entirely as compiled native code, never touching the interpreter.

### How they compose

```
Dispatch (branch to overlay address):
  1. Static dispatch table  [Layer B]  → precompiled function     zero runtime cost
  2. Compiled cache         [Layer A]  → LoadLibrary'd function   one-time per session after first
  3. Dirty-RAM interpreter             → fallback                 first-ever encounter only
```

Layer B coverage grows with every rebuild. Layer A covers the gap within a session before the
rebuild happens. Dirty-RAM fires exactly once per undiscovered overlay.

---

## Game identification

Cache and capture files are namespaced by SCUS/SLUS/SCES product code, read from `SYSTEM.CNF`:

```
cache/
  SCUS-94236/          ← Tomba (NTSC-U)
    <hash>.dll
logs/
  SCUS-94236/
    overlay_map.jsonl
overlay_captures.json  ← written at exit, next to the executable
```

---

## Cache invalidation

A cached DLL is keyed by the hash of the overlay bytes. The hash IS the identity. If a disc dump
differs between users or regions, the hash differs and the old cache entry is simply never
matched. No explicit invalidation needed.

When Layer B statically compiles an overlay, Layer A's cached DLL for that hash becomes
unreachable (static dispatch wins at step 1). It can be pruned post-build but is benign if left.

---

## Implementation phases

| Phase | Work | Status |
|---|---|---|
| — | Dirty-RAM interpreter (Layer A last-resort fallback) | Done |
| B-1 | CD DMA byte capture ring (always-on, retail-safe, `dest < 0x1C0000` filter) | To build |
| B-1 | Automatic `overlay_captures.json` written at process exit | To build |
| B-2 | `game.toml [[overlays]]` + `psxrecomp-game` overlay compilation | To build |
| B-3 | Contribution ingestion script: JSON → `.bin` + `game.toml` entries | To build |
| A-1 | On DMA completion, check cache DLL; if present, `LoadLibrary` + register | To build |
| A-2 | Background thread: emit C, compile to DLL, write to cache | To build |
| A-3 | On next overlay encounter, A-1 fires instead of dirty-RAM | Follows from A-1/A-2 |

B-1 through B-3 ship before A-1. The system is useful with only Layer B (requires a rebuild
after each new playthrough contribution). Layer A eliminates the "rebuild required" friction and
makes the game self-healing for any user.

---

## Scatter-load discovery (Tomba)

Tomba's CD DMA log (captured 2026-05-30) confirmed the scatter-load format:

- Every sector pair: `12-byte block descriptor` (always to a header buffer at `0x973C0`) +
  `2048-byte data block` (scattered to the address the descriptor specifies).
- A single ISO file load produces DMA entries with non-contiguous destination addresses.
- The 12-byte descriptor encodes the block's destination; the loader dispatches accordingly.

This format is game-specific but the DMA capture approach handles it transparently: the captured
bytes are the post-scatter RAM contents, which is exactly the execution form the recompiler needs.

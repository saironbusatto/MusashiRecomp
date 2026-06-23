# Overlay Cache v2 — rearchitecture spec

Status: in progress (started 2026-06-23, branch `wt/tomba2`).
Driver: the overlay cache is net-negative on a cold cache (capture/JIT/CRC
overhead with little native payoff) and wasteful on a warm one (redundant
builds). This spec rebuilds the *efficiency* layer while KEEPING the
correctness model, which the code audit proved is already sound.

## What the audit found (keep vs fix)

KEEP — the correctness model is good:
- **Content-validated dispatch.** Every native overlay call re-hashes live RAM
  (`cand_crc`) and runs the native fn ONLY when `live == crc_code`
  (`overlay_loader.c:1102-1106`, call at 1161; CPS path 1062-1081). A stale
  provider can never execute against mismatched RAM. This is the right model.
- **Additive variant candidates.** Multiple candidates per guest address, chained
  by `Candidate.next`, content-keyed at dispatch. Same address holds scene A's
  code now / scene B's later; the matching variant wins. Correct.
- **Per-function `code_crc` already exists** in the `.ranges` manifest
  (`F <entry> <code_crc>`, `compile_overlays.py:1066-1083`) — the fine-grained
  identity we need is already computed; it's just not used as the cache key.
- **gcc compile is out-of-process** (`autocompile.c` spawns `cmd.exe /C`).

FIX — the efficiency layer:
1. **Build/dedup key = whole-region CRC32** (`compile_overlays.py:1241,1283`).
   Any volatile byte in the captured region (animation counters, data tables)
   changes the region CRC → new DLL filename → recompile, even when every
   compiled function is byte-identical. Piles of redundant DLLs; skip never fires.
2. **Per-dispatch full CRC32 tax.** `cand_crc` CRC32s the function's entire code
   body on EVERY dispatch (`overlay_loader.c:1102`). The page-generation counter
   (`val_gen`/`cand_gensum`/`overlay_watch_set_range`) that could gate this is
   demoted to diagnostic (comment at 1098-1101). Hot per-frame code pays a full
   CRC32 per call — a real drag that blunts the native win.
3. **Main-thread work.** Capture serialize (`overlay_capture_write_json`:
   memcpy + base64 of the whole region), `overlay_loader_rescan` (dlopen + parse
   + register, via `autocompile_poll_main`), and sljit JIT (`try_sljit_region`,
   synchronous on the dispatch thread) all run on the emu/main thread → hitches.
4. **No in-flight dedup** beyond the global `AC_RUNNING` lock (`autocompile.c:21`).

## Target architecture (per ChatGPT review + audit)

Reframe the question from "have I built this region?" to "do I have a native
provider for every function identity present right now?"

- **Logical identity = FuncKey** (per function): `{game_id, guest_entry_vaddr,
  extent_start, extent_len, code_hash, recompiler_version, codegen_abi_version}`.
  Guest address is PART of the key (PC-relative branches / delay-slot lowering
  mean same bytes at a different address are NOT equivalent). code_hash stays
  CRC32 for now (matches runtime `crc32_compute`; revisit to xxHash128 later — a
  separate change since the runtime hash must match).
- **Build/package unit = bundle** of many FuncKeys → one DLL.
  `BundleKey = hash(sorted FuncKey list, codegen_ver, flags)`. Region CRC becomes
  provenance metadata only, not a filename key.
- **Provider index** (persistent): `FuncKey -> {bundle, export, quality, ...}`.
  Build decision: for each discovered function, if a valid provider OR an
  in-flight job already exists for its FuncKey, skip; else add to the missing set;
  one bundle build for the missing set. Kills redundant builds.
- **Dispatch validity = generation-gated.** Promote `val_gen`: if the page
  generation is unchanged since last validation, trust the prior result and skip
  the CRC32; only re-hash when the generation changed. Removes the per-dispatch
  CRC tax for stable code. (The full-CRC remains the source of truth on gen change
  and on first run.)
- **Async publish.** Main thread only: check dispatch cell, enqueue by FuncKey,
  interpret. Off-thread: capture serialize, gcc spawn, dlopen, register; publish
  is an atomic swap that re-validates live bytes first. sljit stays a temporary
  low-priority provider; a later gcc bundle promotes over it.
- **In-flight dedup.** Concurrent `FuncKey -> status{Unknown,Queued,Compiling,
  Ready,Published,Failed}` map; main-thread miss enqueues only if no provider and
  not already in flight.

## Phasing (each phase independently testable; commit per phase)

- **P1 — Re-key build/cache by per-function identity.** compile_overlays.py emits
  bundles keyed by the FuncKey set (not region CRC); per-function skip via the
  provider index; loader scan reads the index. Validate: a volatile-region replay
  produces ZERO new builds when code is unchanged. (Biggest "stop building junk"
  win; no runtime-correctness risk.)
- **P2 — Generation-gated dispatch validation.** Promote `val_gen` to the fast
  path; CRC only on gen change. Validate: byte-identical native behaviour
  (shadow-diff clean) + measurable emu_cpu drop on warm hot code.
- **P3 — Async/off-main-thread.** Move capture serialize + rescan/dlopen off the
  emu thread; atomic publish. Validate: no frame hitches on capture/compile.
- **P4 — In-flight dedup** keyed by FuncKey.
- **P5 — Validate end to end.** Clean warm of Tomba 2, shadow-diff to catch any
  codegen divergence (the GP0 0xFE suspect), measure emu_cpu → target ~16ms.

## P-region-start fix + P5 divergence findings (2026-06-23)

- **THE slowness root cause (FIXED, psxrecomp bb62b20).** `try_load_region`'s
  dirty-page walkback overshot `OVERLAY_REGION_FLOOR` (the boot-EXE image is
  dirty and contiguous below the floor), computing `region_start=0x10000` while
  the capture/DLL filename uses `0x38000`. So the hot overlay region's DLLs were
  built+indexed but NEVER loaded — hot code (`0x50CE8`, >1e9 interp insns) could
  never go native. Clamp the walkback to the window floor. Effect: loads 2→4,
  native funcs 14→790.

- **Remaining blocker: native-vs-interp divergence at `0x80051FA4`** (shadow-diff
  pinned it; same address was in the native ring before the GP0 0xFE crash, so
  likely the same root). With native on, the boot hangs at frame ~1828;
  `overlay_native_off` → ~67fps.

- **The alias/interior-entry keying smell (user-identified).** `0x80051FA4` is a
  CPS CONTINUATION point inside host `0x80051F80` (block_80051F80 does a CPS
  `jal 0x80080880` with return addr 0x51FA4; block_80051FA4 is the
  continuation/epilogue). The recompiler emits BOTH:
    - `func_80051F80` — the full 2-block host (with an entry-switch routing
      `pc==0x51FA4 -> block_80051FA4`), AND
    - `func_80051FA4` — an alias stub `psx_alias_body_80051F80(cpu, 0x51FA4)`
      whose body contains ONLY block_80051FA4 (the epilogue).
  In the `.ranges` BOTH carry the SAME range (`R 80051F80 34`) and the SAME
  `code_crc` (`5CE0B05E`). So the loader registers TWO candidates covering the
  same bytes — and a CPS continuation to 0x51FA4 can resolve via idx_head (the
  alias, epilogue-only) OR overlay_find_by_range (either candidate, same range).
  Hypothesis to confirm with the recompiler/ChatGPT: aliases/interior entries
  should ROLL UP to the parent host shard (same range+crc => deduplicate, register
  the parent and let overlay_find_by_range handle the interior PC), instead of
  emitting a separate epilogue-only alias body that can be dispatched in place of
  the full function. The divergence/hang is very likely this dual-compilation +
  duplicate-candidate interacting with the CPS continuation path.

## Notes
- The GP0 0xFE crash was NOT stale-provider execution (dispatch re-validates).
  It was either the stale/mixed cache (now wiped) or a real codegen divergence;
  P5 shadow-diff isolates it.
- code_hash stays CRC32 in v2 (runtime `crc32_compute` must agree byte-for-byte);
  upgrading the hash is a coordinated tool+runtime change, deferred.

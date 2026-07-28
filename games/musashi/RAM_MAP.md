# Brave Fencer Musashi (SLUS-00726) — RAM map

Addresses found by live differential scanning against a real playthrough:
snapshot the full 2 MB over the debug server's `read_ram`, have the player take
damage, snapshot again, and keep only the addresses that changed from the old
displayed value to the new one. Two rounds took 2,097,152 bytes down to two
candidates.

All addresses are offsets into main RAM as `read_ram` reports them (add
`0x80000000` for the KSEG0 view the game itself uses).

## Player stats block

Found 2026-07-27. Confirmed against the on-screen HUD at two different HP
values (37 → 27) and against BP reported independently by the player.

| Address    | Size | Meaning     | Observed | 
|------------|------|-------------|----------|
| `0x078EB2` | u16  | **HP maximum** | 150   |
| `0x078EB4` | u16  | **HP current** | 37 → 27 |
| `0x078EB6` | u16  | **BP maximum** | 150   |
| `0x078EB8` | u16  | **BP current** | 147   |

Four consecutive `u16`s, `[max, current]` for each stat.

**Settled by decompiled code, after I got it wrong once in between.** The first
labelling above was inferred from the HUD. I then "corrected" it to make
`0x78EB6` the HP maximum, on the strength of `FUN_8014BCEC` adding to `0x78EB6`
with a cap — without checking what actually clamps against it. That was wrong,
and the original was right. The code is unambiguous:

```c
/* 0x78EB2 is the HP ceiling */
if (DAT_80078eb4 == DAT_80078eb2)          /* HP full? */
if (DAT_80078eb4 <= DAT_80078eb2 >> 1)     /* below half? */
DAT_80078eb4 = DAT_80078eb2;               /* full heal */
if (DAT_80078eb2 < DAT_80078eb4) DAT_80078eb4 = DAT_80078eb2;   /* clamp */
DAT_80078eb2 = DAT_80078eb2 + n;           /* grow max HP */

/* 0x78EB6 is the BP ceiling */
FUN_8014BD24: DAT_80078eb8 += n; if (DAT_80078eb6 < DAT_80078eb8) DAT_80078eb8 = DAT_80078eb6;
FUN_8014BDC8: DAT_80078eb8 = DAT_80078eb6;      /* refill BP */
FUN_8014BCEC: DAT_80078eb6 += n, capped 0x662;  /* grow max BP */
```

Both maxima read 150 at observation time, so the HUD alone could never separate
them — only the code could. Lesson: partial code reading is not better than
inference, it just looks more authoritative.

### Two identical copies

The same four-value block appears at **both** `0x078EB4` and `0x11F82C`, with
byte-identical surroundings for at least ±64 bytes. Both tracked the HP change
together, so a differential scan cannot separate them.

Which one the game actually reads is **not yet determined**. The test is to
write one and watch the HUD: if it updates, that copy is live. It was not run
because the only instance available was the player's own session and writing
into it uninvited was not worth the information.

Working hypothesis (unverified): one is the live struct and the other a
save/serialisation buffer or a double-buffer. Do not assume.

### Not in this block

Money (1500) and the day counter are **not** within ±96 bytes of either copy —
they live somewhere else and still need their own scan.

## Method notes

- `read_ram` accepts up to `0x200000` in one call, so a whole-RAM snapshot is a
  single request; diffing locally is far faster than iterative narrowing.
- `write_ram` takes **one byte** per call (`val` is `uint8_t`), so a u16 needs
  two calls — low byte then high byte.
- Pad input for `press` is **active low** (`0 = pressed`), per `sio.h`: all
  buttons released is `0xFFFF`, START alone is `0xFFF7`. Getting this backwards
  silently does nothing.
- Do not inject input into a session someone is playing. `clear_input` releases
  an override.

## Damage: the writer, and proof that overlay code is patchable

Found 2026-07-27 with `wtrace_range` armed on the HP address while the player
took hits.

Six writes, all from the same instruction:

| HP before → after | `a1` |
|---|---|
| 140 → 130 | 10 |
| 130 → 125 | 5  |
| 125 → 101 | 24 |

`a1` is the damage amount, 3/3. `v0` carries the resulting HP.

```
0x8014BCB4   A4 22 8E B4   sh $v0, 0x8EB4($at)     <- applies damage
0x8014BC70                 return address of the caller
```

### This is overlay code, not the static EXE

```
static EXE text ends : 0x80074800   (load 0x80010000 + text_size 0x64800)
damage writer        : 0x8014BCB4   (far past it)
```

It is in **neither** the 4386 functions our recompiler discovered nor the 3697
in the Ghidra corpus — both cover only the static EXE. The game keeps real
gameplay logic in overlays streamed from disc.

### Runtime patching works (verified)

Overwrote the instruction at `0x8014BCB4` with `NOP` (four zero bytes) via
`write_ram` during live play. The player's HP then **froze at 65 and stopped
responding to hits entirely** — not healing, the store simply never executes.

That settles the question the `--wrap` plan could not answer: **overlay code can
be modified at runtime by writing MIPS instructions straight into RAM**, and the
dirty-RAM interpreter (CLAUDE.md rule 18) executes the patched code. Classic
ROM-hacking, and it works uniformly for static and overlay code.

The patch lives only in RAM, so it disappears when the process exits. Nothing to
undo, and no save is touched.

### What this means for the mod architecture

`--wrap` at link time only reaches statically recompiled functions, so it cannot
touch where BFM's gameplay logic actually lives. A RAM patch can. Any permanent
mod therefore needs a load-time hook that applies patches once an overlay is
resident, not a link-time wrap — that design question is still open.

## Savestates

`{"cmd":"savestate","op":"save"|"load","slot":N}`. Deferred to the main loop, so
the file appears a frame or two later; the runtime logs `savestate: SAVED slot N`.
Files land next to the executable as `state_<entry_pc>_slot<NN>.pst` (~3.6 MB).

Loading a savestate **restores overlay code into RAM** — verified by reading
`0xA4228EB4` back at `0x8014BCB4` after a load. That makes savestates a way to
reach overlay code for analysis without replaying the game.

Existing slots: 1 = pre-Lumina, 2 = post-Lumina.

## Overlay function corpus

The static-EXE corpus in `games/musashi/ghidra/` does not cover overlays, which
is where the gameplay logic lives. Extracted separately:

1. Boot, load a savestate (restores overlay code into RAM), dump RAM.
2. Slice from `overlay_region_floor` = `0x74800` to the end of RAM — 1,619,968
   bytes, 52% non-zero.
3. Import into Ghidra as a raw binary based at `0x80074800`, analyze, decompile.

Result: **2642 functions, all decompiled** — `games/musashi/ghidra_overlay/`.
Note this is one session's overlay residency, not every overlay in the game;
visiting new areas and repeating the dump extends coverage.

### Stat routines (the first real mod targets)

| Address | Decompiled behaviour |
|---|---|
| `0x8014BC80` | `HP -= dmg`; if `dmg > HP` then `HP = 0` and clears `0x800B9A17` (death) |
| `0x8014BCC0` | `HP -= dmg` but floors at **1** — damage that cannot kill |
| `0x8014BCEC` | adds to **BP** maximum `0x78EB6`, clamped to `0x662` (1634) |
| `0x8014BD24` | adds to BP `0x78EB8`, clamped to BP max `0x78EB6` |
| `0x8014BD60` | subtracts from BP `0x78EB8`, floors at 0 |
| `0x8014BDC8` | refills BP to maximum |

`0x8014BCB4`, the store found by `wtrace`, is inside `0x8014BC80`. The two
distinct damage routines (killing vs non-killing) are a useful distinction for a
PvP mod: an arena that should never hard-kill can route through the second.

Mod handles this gives, all by RAM patch:
- invulnerability — `NOP` the store, verified working
- damage scaling — rewrite the subtract, or clamp the incoming argument
- one-hit kills — force the death branch

### Caveat

Overlay addresses are only stable while that overlay is resident. A patch must
be applied *after* the overlay loads and re-applied if it is evicted and
reloaded. Verifying that the address still holds the expected instruction before
patching is mandatory, not optional — patching whatever happens to occupy the
address after an eviction would corrupt unrelated code.

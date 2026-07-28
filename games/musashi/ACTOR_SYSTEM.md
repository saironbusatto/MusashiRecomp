# Brave Fencer Musashi (SLUS-00726) — actor system

From the overlay corpus in `ghidra_overlay/` plus the static-EXE corpus in
`ghidra/`. Addresses are KSEG0 as the game uses them.

**This is the map that decides the multiplayer go/no-go.** The short version:
there is a 96-slot generic actor pool, the player is *not* in it, the input
routine already takes a controller port number, and player HP/BP are globals
rather than struct fields. That last one is the problem.

## Two separate systems

### 1. Generic actor pool — confirmed

```
base   0x801202A0
end    0x80126720   (exclusive)
stride 0x10C
count  96           (0x6480 / 0x10C)
```

Confirmed three independent ways: the walk in `FUN_8012C51C`
(`for (; p != &DAT_80126720; p += 0x86)` from `&DAT_801202A0`), the whole-pool
clear `func_0x80016714(&DAT_801202A0, 0x6480)` in `FUN_80178608`, and the
single-slot clear `func_0x80016714(param_1, 0x10C)` in `FUN_8012C218`.

Field offsets (high confidence unless noted):

| Off | Type | Meaning |
|---|---|---|
| `0x00` | u16 | type / vtable index. **0 means the slot is free** |
| `0x02` | u16 | per-type sub-state |
| `0x04/08/0C` | 3×s32 | position, 16.16 fixed |
| `0x20` | ptr | render/model object |
| `0x36` | u16 | unique instance id, from counter `DAT_801270C4`, used for stale-handle checks |
| `0x38/3C/40` | 3×s32 | previous position |
| `0x64` | ptr | owner / target actor |
| `0x88/8A/8C` | 3×s16 | home position |
| `0xAA` | s16 | spawn-in scale timer — **note this collides with a different meaning in the player struct** |
| `0xC0/0xC1` | u8 | behaviour override pair |

Medium confidence: `0x52`, `0x58`, `0x5C`, `0x68`, `0x76/0x78`.

### 2. Player — a separate singleton, not a pool slot

```
address 0x80126B58
size    0x254        (memset in FUN_80145CEC)
```

It sits past the pool end but is never passed to any pool-generic routine
(`FUN_8012C51C`, `FUN_8012DE2C`, `FUN_8012E504`, `FUN_8012C218`, `FUN_8012C890`
— zero references). It shares a prefix layout with pool actors up to roughly
`0xA8`, then diverges.

| Off | Meaning |
|---|---|
| `0x00` | s16 type, same enum space as the pool (`0x1A`/`0x1E` are special-cased modes) |
| `0x04/08/0C` | position, 16.16 fixed |
| `0x20` | model object ptr |
| `0x44` | u32 flags (triple-confirmed) |
| `0x48` | previous-frame copy of `0x44` |
| `0x4C` | state index into jump table `PTR_FUN_8017FE70` |
| `0x68/6A/6C` | scale (`0x1000` = 1.0) |
| `0xA9` | u8 pad type (`0x41` digital, `0x53`/`0x73` analog) |
| `0xAA` | u16 buttons bank A |
| `0xAC` | u16 buttons bank B |
| `0xAE` | u16 analog, packed `x \| y<<8` (`0x8080` centred) |
| `0xF2` | s16 pending incoming damage |
| `0x1B0` | s32 "controls inverted" |
| `0x1C5` | u8 input disable |
| `0x1DC/1E0/1E4/1E6` | button edge detection: prev, current, countdown, reload |

## Frame order

Player update runs to completion, then the actor pool:

| Role | Address |
|---|---|
| per-frame tick (3 mode variants) | `0x801285E4`, `0x80128678`, `0x80128714` |
| player update (whole) | `0x8014607C` |
| ├ pre-update / physics / collision | `0x80146128` |
| ├ state dispatch via `PTR_FUN_8017FE70[state]` | `0x80146360` |
| └ post-update / transform writeback | `0x801463A0` |
| rest-of-world tick | `0x801287B8` |
| **actor pool walk (96 slots)** | **`0x8017849C`** |
| pool spawn: free-slot scan | `0x8012C51C` |
| pool spawn: constructor | `0x8012C890` |
| pool despawn | `0x8012C218` / `0x8012CAE4` |
| pool init / full reset | `0x80178608` |

Pool dispatch is `(**(code **)(PTR_PTR_8011DB08 + type*4))(slot)`, and
`PTR_PTR_8011DB08` is loaded from level data at runtime.

## The input seam

```c
void FUN_80148648(short *actor, u8 padPort);   // 0x80148648
```

Called as `FUN_80148648(&DAT_80126B58, 0)`. **The second argument is already a
controller port index**, and it flows through to a per-port array in the static
EXE:

```
pad slot base 0x80078D98, stride 0x4C, indexed by port
  +0x00  u8   pad type
  +0x32  u16  buttons bank A
  +0x3A  u16  buttons bank B
```

Port 1 already exists and is already polled — `FUN_80014CAC` reads
`_DAT_80078DCA | _DAT_80078E16`, and `0x80078E16` is exactly
`0x80078DCA + 0x4C`, i.e. pad 1. The menu code ORs both pads together.

D-pad bits (high confidence): `0x8000` LEFT, `0x2000` RIGHT, `0x1000` UP,
`0x4000` DOWN, `0x0008` START. Face buttons medium confidence.

## The obstacle

`FUN_8014BC80` and its damage siblings take a `param_1` that they **ignore** —
they operate on the globals `DAT_80078EB4/EB6/EB8`. So player HP, HP max and BP
are **not fields of the player struct**. A second player instance would share
one health pool with the first unless those globals are also virtualised.

Second obstacle, same class: parts of the player pipeline reach the singleton
through absolute globals rather than through `param_1`. `FUN_80146360` and
`FUN_801463A0` are both hardcoded to `&DAT_80126B58`. Others
(`FUN_8014B190`, `FUN_80149228`, `FUN_8014A59C`, `FUN_80148648`) are fully
`param_1`-relative and would work on a second instance unmodified.

**How reusable the pipeline is has not been measured.** The audit is mechanical:
for every callee of `FUN_80146128` and `FUN_801463A0`, check whether it touches
any `DAT_80126xxx` global. That number is the real go/no-go input.

## Not determined

- Enemy/NPC HP. No damage-application path for pool actors was found; the
  `FUN_8014BC80` family is player-only. Where enemy health lives in the 0x10C
  struct is unknown.
- Draw/sort submission. Position writeback into the object at `+0x20` is
  confirmed; the routine that submits to the GPU ordering table was not traced.
- Contents of `PTR_PTR_8011DB08` and `PTR_FUN_8017FE70`. Both are pointer data
  loaded at runtime, so enumerating actor types and player states needs a memory
  dump, not this corpus.
- Whether the pool holds enemies specifically or only props/projectiles/NPCs.
- Camera ownership, and whether the render path could serve two viewpoints.

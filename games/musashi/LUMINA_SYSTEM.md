# Brave Fencer Musashi (SLUS-00726) — Lumina / ability system

From the overlay corpus (`ghidra_overlay/`, base `0x80074800`) and the static
EXE corpus (`ghidra/`). This is the mechanic the arena mod is built around, so
the important question throughout is **what is per-player and what is global**.
Short answer: all of it is global.

## Two separate systems

### 1. Assimilation — one slot, not a collection

`0x80078EC1` holds the **currently held assimilated ability id**, 0..0x1E
(0 = none). There is no "owned abilities" set: absorbing overwrites the slot.
That matches the retail game — you carry one absorbed ability at a time.

The `0x1E` ceiling is explicit in the absorb routine `FUN_80163EC8`:

```c
uVar4 = FUN_8014CA00(iVar6);        /* the enemy's ability id */
*(uint *)(psVar5 + 0xc2) = uVar4;
if (0x1e < uVar4) { psVar5[0xc2] = 0; }   /* > 30 => reject */
```

Related slots in the same block: `0x80078EC2` (mirror), `0x80078EC3/EC4/EC5`
(state/aux, `EC5 = 0x80` means "blocked, not enough BP"), `0x80078EC8` (charge
counter). The player-object copy is `player + 0x1A0`.

Cleared by `FUN_80165A50` and on death by `FUN_8015FBE0`.

### 2. Sword/scroll powers — six, owned via the global event-flag bitfield

`0x80078EC0` low 7 bits hold the active sword power, 1..6. Ownership is a bit in
the **event-flag bitfield at `0x800AE648`** (512 bits), through the static-EXE
accessors:

```c
/* 0x80029124 */ void FUN_80029124(uint id, char on);   /* set/clear bit */
/* 0x80029178 */ bool FUN_80029178(uint id)
    { return (*(byte *)((id >> 3) + 0x800ae648) & 1 << (id & 7)) != 0; }
```

The pause menu (`FUN_8013F350`) gates the six powers on the flag ids in
`DAT_8017F41C[0..5]`. A concrete grant site, `FUN_8014BEC0`:

```c
DAT_80078ec0 = DAT_80078ec0 | 1;
func_0x80029124(0x80, 1);           /* flag 0x80 = has power 1 */
```

## Ability tables — 31 entries, indexed by `id & 0x7F`

Eight parallel tables in overlay data. Entry counts are confirmed by the address
gaps and match the `0x1E` bound.

| Base | Stride × count | Meaning |
|---|---|---|
| `0x80180FE4` | 4 × 31 | use/tick handler jump table |
| `0x80181060` | 1 × 32 | bit7 = per-frame tick; **bits 0-6 = BP cost** |
| `0x80181080` | 4 × 31 | initial charge count → `0x80078EC8` |
| `0x801810FC` | 1 × 32 | absorb-gauge increment per hit |
| `0x8018111C` | 1 × 32 | BP refund on failed absorb |
| `0x8018113C` | 1 × 32 | post-absorb spawn flag |
| `0x8018115C` | 4 × 31 | handler indexed by `player + 0x1A0` |
| `0x801811D8` | 4 × 31 | teardown handler |

Sword powers use separate 6-entry tables at `0x80181A10` / `0x80181A28`, indexed
`0x80078EC0 - 1`.

**The table contents are not in this corpus** — the overlay's data section is
neither in the decompilation nor in `SLUS_007.26`, only the code that indexes
it. Ability id → name, and the per-ability BP costs, must be read from RAM at
runtime once the overlay is resident.

## Key functions

| Address | Role |
|---|---|
| `0x80165580` | **use dispatch** — `(*(code *)(&PTR_LAB_80180FE4)[DAT_80078EC1 & 0x7F])()`, or fizzle if `EC5 & 0x80` |
| `0x801654A8` | per-frame tick, called from the player update `FUN_80146128` |
| `0x80165AC8` | teardown dispatch |
| `0x80165624` | affordability test — returns true when `BP < cost` |
| `0x801655E4` | pay the cost → `FUN_8014BD60` |
| `0x801653F4` | affordability latch, sets `EC5 = 0x80` when blocked |
| `0x8014CA00` | **enemy → ability id**: `*(short *)(*(int *)(enemy + 0x78) + 0x2C)` |
| `0x80163C2C` | grab: stores the locked target pointer in `player + 0x184` |
| `0x80163EC8` | drain loop: `player + 0x188` is the gauge, cap `0x80` |
| `0x8016432C` | marks failure by setting bit31 of `player + 0x184` |
| `0x8015FF20` | **commit — "you got the ability"**, writes `DAT_80078EC1` |

The commit, which is the single most useful hook for the mod:

```c
uVar2 = *(uint *)(param_1 + 0xc2);         /* player + 0x184 */
if ((int)uVar2 < 0) {                      /* bit31 => absorb failed */
    ... BP consolation ...
} else {
    *(char *)(param_1 + 0xd0) = (char)uVar2;   /* player + 0x1A0 */
    DAT_80078ec1 = (undefined1)param_1[0xd0];  /* <== ACQUIRED */
    DAT_80078ec2 = DAT_80078ec1;
    FUN_80165670(param_1, DAT_80078ec1);       /* load charge count */
}
```

`FUN_8015FF20`, `FUN_8015FCC8`, `FUN_80163C2C`, `FUN_80163EC8` and `FUN_8016432C`
have no direct callers in the corpus — they are entries in the player
state-machine table in overlay data, consistent with a multi-phase absorb
animation.

## What this means for the arena mod

**Everything is singleton global state**: the held ability (`0x80078EC1`), BP
(`0x80078EB8`), the sword power (`0x80078EC0`), the block latch (`0x80078EC5`),
the charge counter (`0x80078EC8`), and the player object itself
(`0x80126B58`). See [ACTOR_SYSTEM.md](ACTOR_SYSTEM.md) — the same problem shows
up there with HP.

The ability tables are read-only and index-addressed, so they can be shared
between players safely.

For N players you would need to replicate the saved player block
(`0x80078E78..0x80078F10`, 0x98 bytes) plus the `0x80126B58` object, and thread
a context pointer through `FUN_80165580`, `FUN_801654A8`, `FUN_801655E4`,
`FUN_80165624` and `FUN_8014BD60`. Those already take a `param_1` they currently
ignore, which is a convenient hook — the same vestigial-argument pattern the
damage routines show.

## Save layout (useful for a per-player state block)

`FUN_8017DF40` serialises exactly these ranges:

| RAM | Size | Role |
|---|---|---|
| `0x80078E78` | 0x98 | player block — HP, BP, `EC0`, `EC1`, … |
| `0x800A6588` | 0x80 | 0x40 u16s |
| `0x800AE648` | 0x40 | event-flag bitfield, 512 bits |
| `0x800BA1B8` | 0x100 | byte variables (item counts) |
| `0x800BA2B8` | 0x60 | 0x18 u32s |

Defaults are installed by `FUN_80029274` from ROM `0x80072C84`.

## Not determined

- Contents of every overlay data table listed above — read them from RAM.
- Meaning of bit7 in `0x80181060`. It gates per-frame ticking; "passive ability"
  is inference, not fact.
- The save side of the `0x80078ECC` stat backup used by `FUN_80165874`.
- The button read that enters ability-use state `0x17`; the state-enter table is
  overlay data.
- Why ids `0x19` and `0x1A` are special-cased (`0x1A` is exempted on map ids
  `0x305C..0x3066`).

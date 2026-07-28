# Go/no-go: can a second controllable character exist?

The agreed pass/fail for the whole multiplayer project is *a second Musashi on
screen, in the same map, moving under injected input*. This measures how far
away that is, from the corpus rather than from opinion.

**Verdict: not blocked. The engine is already written multi-instance; what is
singleton is the state, not the code.**

Measured over the transitive call closure of the player update
(`FUN_8014607C`, `FUN_80146128`, `FUN_80146360`, `FUN_801463A0`) in the overlay
corpus — 94 functions.

## 1. The pipeline is 95% ready

| | count |
|---|---|
| functions in the player pipeline | 94 |
| already fully `param_1`-relative — work on another instance unmodified | **89 (95%)** |
| actually referencing the player struct | **3** |

The three:

| Function | refs to `&DAT_80126B58` | of which are *just passing the pointer* |
|---|---|---|
| `FUN_80146128` | 28 | **28** |
| `FUN_80146360` | 1 | **1** |
| `FUN_801463A0` | 9 | **9** |

**Every single reference is passing `&DAT_80126B58` as an argument.** None of the
three reads or writes a field through the constant. So making the pipeline
multi-instance is mechanically: replace the constant with a context pointer in
three functions. In MIPS that is a `lui/addiu` pair per site.

(`FUN_8016F1AC` and `FUN_8016F1C4` show up in a naive `0x80126xxx` scan but
touch `DAT_80126D50`, which is outside the 0x254-byte struct. Not player state.)

## 2. Input is already port-parameterised

`FUN_80148648(actor, padPort)` takes the controller port as an argument, and pad
1 is already polled by the hardware layer (`FUN_80014CAC` reads
`_DAT_80078DCA | _DAT_80078E16`, and `0x80078E16` is `0x80078DCA + 0x4C`, one
stride along the pad array). Nothing needs inventing here — the second player's
input path exists.

## 3. The real work: 19 globals

The pipeline touches **19 distinct addresses** inside the saved player block
(`0x80078E78..0x80078F10`), across **141 references**.

| Address | Meaning | refs | functions |
|---|---|---|---|
| `0x80078EA4` | unidentified | 22 | 5 |
| `0x80078EB4` | HP | 17 | 3 |
| `0x80078ED8` | unidentified | 16 | 1 |
| `0x80078E94` | unidentified | 11 | 4 |
| `0x80078EB8` | BP | 10 | 4 |
| `0x80078E90` | unidentified | 9 | 1 |
| `0x80078EC1` | assimilated ability | 7 | 5 |
| `0x80078ECC` | stat backup | 7 | 1 |
| … | 11 more | | |

These are plain globals, so two players would share one health pool, one ability
slot and one charge counter. **This is the actual obstacle** — not the actor
pool, not input, not the update loop.

## What that implies

The shape of the work is now known, which is what a go/no-go is for:

1. Allocate a second `0x254` player struct and a second copy of the
   `0x80078E78..0x80078F10` block.
2. Patch three functions to take a context pointer instead of the constant.
3. Virtualise the 19 globals — the honest unknown. Options range from swapping
   the block in and out around each player's update (crude, works, costs a
   memcpy per player per frame) to rewriting the 141 references to index off a
   context register (clean, far more patching).
4. Call the pipeline twice per frame, once per player, with port 0 and port 1.

Step 3 is where this could still fail, and the swap approach is the cheap
experiment that answers it: if running the pipeline twice with the state block
swapped produces two independently-moving characters, the go/no-go passes.

## Still unknown

- Whether the renderer can present two player models. Position writeback into
  the model object at `+0x20` is per-actor, which is encouraging, but the
  submission path was not traced.
- Camera ownership with two players.
- What the 11 unidentified globals in the block do; some may not need
  duplicating at all.
- Whether the player state machine (`PTR_FUN_8017FE70`) has any handler that
  reaches for the singleton directly. The table is overlay *data*, absent from
  the corpus — it has to be read from RAM.

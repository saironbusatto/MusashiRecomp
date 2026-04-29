# Memory-Card End-to-End Audit — 2026-04-28

Branch: `feature/cycle-paced-sio-irq` @ commit `5b3c7dd`
Date: 2026-04-28
Operator: full audit run on a freshly-rebuilt native runtime (`psx-runtime.exe`).

End-to-end success criterion: real `.mcr` save data (e.g. the
`BISLPS-01234SAVE00` save title in `card1.mcd`) appears as a list entry
on the BIOS MEMORY CARD screen, having travelled through the real SIO
protocol — no HLE shims, no fake directory entries, no edits to
generated code.

This audit walks all seven layers, fills the table, and identifies the
first failed layer with a fix proposal scoped to it. No source change
beyond two infrastructure additions made during the audit
(`tools/debug_client.py` JSON-read fix; new `card_buffer_dump` TCP
command).

---

## Audit table

| # | Layer | Component | Expected | Observed | Evidence | Verdict |
|---|---|---|---|---|---|---|
| 1.1 | L1 | `bios/dummy.0.mcr` exists | exists per session prompt | does NOT exist | `ls -la bios/` shows only `SCPH1001.BIN`, `dummy.bin`, `dummy.cue`, `scph5501.bin` | **FAIL (prompt-only — informational)** |
| 1.2 | L1 | All `.mcr`/`.mcd` files are 131072 B | 131072 | all four are 131072 B | `ls -la card1.mcd card2.mcd dummy.0.mcr dummy.1.mcr` | PASS |
| 1.3 | L1 | "MC" magic at offset 0 | `4D 43` | `4D 43` on all four | `xxd -s 0 -l 0x80` on each | PASS |
| 1.4 | L1 | Frame 1 dir entry semantics | "used" or "free" with valid checksum | card1.mcd: 2 used (BISLPS-01234SAVE00, BASLUS-56789DATA01); card2.mcd: empty; dummy.0.mcr: 2 used (same first two as card1 but blocks 3-15 differ); dummy.1.mcr: 1 used (BASLUS-0125100000-00) | hex dump | PASS |
| 1.5 | L1 | Native source = Beetle source (sha256) | byte-equal | sha256 differs across all four pairs (even the first 16-frame directory region differs) | `sha256sum card1.mcd dummy.0.mcr` → `f79bfc41…` vs `e146de7c…` | **FAIL (test-setup, blocks oracle byte-diff)** |
| 2.1 | L2 | slot 0 in-memory buffer = `card1.mcd` on disk | byte-equal sha256 | MATCH | `python tools/verify_card_buffer.py` slot 0 sha = disk sha = `f79bfc41…` | PASS |
| 2.2 | L2 | slot 1 in-memory buffer = `card2.mcd` on disk | byte-equal sha256 | MATCH | same script slot 1 sha = `7706c7d4…` | PASS |
| 3.1 | L3 | Card transactions occur after entering MC screen | ≥1 closed READ txn | 542 attempted | `card_txn_dump count=10000` yields 542 entries | PASS |
| 3.2 | L3 | Each READ TX byte sequence matches protocol | 0x81, 0x52, 0x00, 0x00, … | first 4 TX correct, txn never gets past byte 4 | `card_txn_dump`: every entry has `tx:[0x81,0x52,0x00,0x00]` | PARTIAL |
| 3.3 | L3 | Each READ RX matches no$psx protocol | 0xFF, 0x00 (flag), 0x5A, 0x5D, 0x00, 0x00, 0x5C, 0x5D, echo_msb, echo_lsb, 128 data, chk, 0x47 | `[0xFF, 0x00, 0x5A, 0x5D]` only — protocol-correct for the first 4 bytes (mc_flag=0 after first read; ID bytes 0x5A 0x5D follow) | `card_txn_dump` rx field | PASS for the bytes seen |
| 3.4 | L3 | Transaction reaches END (terminal_state=12) | success | **all aborted** — 537 / 542 = `abort_reselect` at terminal_state=4 (MC_ADDR_MSB); 6 = `abort_reset` at terminal_state=0; 1 open; 0 success | `card_txn_dump` aggregate (see transcript A) | **FAIL** |
| 3.5 | L3 | RX matches Beetle byte-for-byte | byte equal | NOT MEASURED — gated on Layer 1 file equality, currently FAIL | (would be `card_txn_diff`) | BLOCKED-BY-L1 |
| 4.1 | L4 | Install handler at RAM 0xCF0 fires per byte | one fire per RX byte | install handler fired 26,030 times (3 insns/fire); chain entry 0x641C fired 26,030 times | `dirty_ram_stats` → per_pc 0xCF0 hits=26030 | PASS (handler runs) |
| 4.2 | L4 | `[0x72F0]` data-byte counter advances 0..127 | monotonic increase to 127 per sector | currently `[0x72F0] = 0`; never observed > 1 in any window | `read_ram addr=0x800072F0 len=4` | **FAIL** |
| 4.3 | L4 | `[0x755A]` abort flag = 0 across a transaction | 0 | currently 0; setter pattern matches prior memo `phase4_real_blocker_2026_04_28.md` (R1/D1 set unconditionally) | `read_ram addr=0x8000755A` (and `wtrace_dump` for slot 7) | needs deeper trace |
| 5.1 | L5 | I_MASK bit 7 (SIO) gets set during chain enter | ≥1 set | bit7_sets=205, bit7_clears=205 — toggled 205× during exception entries | `imask_trace count=64` | PASS |
| 5.2 | L5 | One IRQ #7 fires per card-protocol byte | 4-138 card IRQs per txn | **0 card-source IRQs in 200,000 most-recent IRQ entries** (full ring) — every recent IRQ is `src=pad` | `sio_irq_dump count=200000 src=1` → `entries:[]`, `emitted:0` | **FAIL — root signal** |
| 5.3 | L5 | `sio_irq_pending` arms after card TX byte | nonzero for ~delay cycles | `sio_irq_countdown` is set to `SIO_IRQ_DELAY_CARD=8` on every TX, but ticks happen only on SIO register access (`sio.c:830,961`) and on interpreter-step (`psx_interpreter.c:517`); between two card TX bursts the BIOS does <8 SIO accesses, so the countdown is rearmed before reaching 0 → IRQ never fires for cards | code read of `sio.c:854-894,974-1008` | **FAIL — proximate cause of 5.2** |
| 6.1 | L6 | `[0x80007568]` slot gates non-zero after card detect | ≥1 byte set | `01 01 00 00 01 …` — both slots flagged | `read_ram addr=0x80007568 len=16` | PASS |
| 6.2 | L6 | `[0x800074BC]` read flag set | 1 during read | `01 00 00 00` | `read_ram` | PASS |
| 6.3 | L6 | `[0x800075C0]` mode word | nonzero during read | `00 00 00 00` (currently idle) | `read_ram` | inconclusive (state machine returned idle by audit time) |
| 6.4 | L6 | `[0x80007550]` buffer ptr set before data phase | nonzero RAM pointer | `00 00 00 00` | `read_ram` | **FAIL** (buffer never set up — read never reached data phase) |
| 7.1 | L7 | MEMORY CARD screen reaches per-card directory listing | 15-row directory grid with save titles | screen shows MEMORY CARD operations menu (EXIT/COPY/COPY ALL/DELETE) only — no per-slot directory listing rendered; CARD 1 and CARD 2 column headers are visible but directory contents are blank | `screenshot_file` → `psx_screenshot.bmp` (see image: `MEMORY CARD operations` menu, no titles) | **FAIL** |

---

## Evidence transcripts

### Transcript A — card transaction outcome distribution

```
$ python tools/raw_tcp.py 4370 card_txn_dump count=10000 | grep -oE '"end_reason":"[^"]+"' | sort | uniq -c
    537 "end_reason":"abort_reselect"
      6 "end_reason":"abort_reset"
      1 "end_reason":"open"

$ python tools/raw_tcp.py 4370 card_txn_dump count=10000 | grep -oE '"terminal_state":[0-9]+' | sort | uniq -c
      7 "terminal_state":0     # MC_IDLE — never even started CMD
    539 "terminal_state":4     # MC_ADDR_MSB — got 4 bytes, then aborted

$ python tools/debug_client.py card_txn_dump count=2 | head -40
{
  "id": 0, "ok": true, "total_closed": 94, "open": true, …
  "entries": [
    {
      "txn_seq": 78, "slot": 0, "cmd": "0x52", "sector": "0xFFFF",
      "bytes": 4, "acks": 1,
      "end_reason": "abort_reselect", "terminal_state": 4,
      "tx": ["0x81", "0x52", "0x00", "0x00"],
      "rx": ["0xFF", "0x00", "0x5A", "0x5D"]
    }, …
```

Note: the `acks` field counts only the SELECT-byte ACK (`sio_mc_ack_count`
incremented at `sio.c:442`; subsequent state transitions set
`SIO_STAT_ACK` but don't increment that counter). It is misleading
without context.

### Transcript B — IRQ ring delivery profile

```
$ python tools/raw_tcp.py 4370 sio_irq_dump count=200000 src=1
{"id":1,"ok":true,"total":200000,"shown":200000,"src_filter":1,"entries":[
],"emitted":0}

$ python tools/raw_tcp.py 4370 sio_irq_dump count=20000 | grep -c '"src": "card"'
0

$ python tools/raw_tcp.py 4370 sio_irq_dump count=20000 | grep -c '"src": "pad"'
~20000   # 100% pad
```

### Transcript C — I_MASK toggle history (bit 7 SIO)

```
$ python tools/debug_client.py imask_trace count=4
…  "bit7_sets": 205, "bit7_clears": 205, …
{"old":"0x04D","new":"0x0CD","func":"0x00005D98","pc":"0xBFC14930","b7s":1,"exc":1}
{"old":"0x0CD","new":"0x04D","func":"0x00001444","pc":"0xBFC14A28","b7c":1,"exc":1}
… (repeated 205 times)
```

I_MASK bit 7 IS being toggled in exception entry — masking is not the
problem. The problem is upstream of mask: the IRQ never fires.

### Transcript D — install-handler aggregate

```
$ python tools/raw_tcp.py 4370 dirty_ram_stats
{"blocks_run":26036, "insns_run":135391627, "aborts":38264, …
 per_pc:[
   {"pc":"0x00000CF0", "hits":26030, "insns":78090},   ← install handler stub
   {"pc":"0x0000641C", "hits":26030, "insns":104120},  ← chain dispatcher entry
   {"pc":"0x00006594", "hits":26024, "insns":26024},
   {"pc":"0x000000B0", "hits":33642758, "insns":100928274},  ← B0 trampoline
   …
 ]}
```

### Transcript E — state-word snapshot

```
$ python tools/raw_tcp.py 4370 read_ram addr=0x80007568 len=16
{"hex":"01010000010000000000000000000000"}     # slot 0 + slot 1 detected
$ python tools/raw_tcp.py 4370 read_ram addr=0x800074BC len=4
{"hex":"01000000"}                              # read-flag = 1
$ python tools/raw_tcp.py 4370 read_ram addr=0x800072F0 len=4
{"hex":"00000000"}                              # data-byte counter = 0
$ python tools/raw_tcp.py 4370 read_ram addr=0x80007550 len=4
{"hex":"00000000"}                              # buffer ptr never set
$ python tools/raw_tcp.py 4370 read_ram addr=0x8000755A len=4
{"hex":"00000000"}                              # abort flag currently clear
```

### Transcript F — visual ground truth

`psx_screenshot.bmp` (after pressing CROSS to enter MEMORY CARD): the
"MEMORY CARD" operations menu is visible with EXIT / COPY / COPY ALL /
DELETE entries and CARD 1 / CARD 2 column headers, but **no per-slot
directory listing is rendered**. The save title "BISLPS-01234SAVE00"
that exists in `card1.mcd` frame 1 does not appear anywhere on screen.

---

## First failed layer

**Layer 3 — transaction completion** is the highest-numbered layer
where the user-visible failure crystallises (transactions truncate at
byte 4 of 138, `mc_read_done = 0`), but the **proximate cause is
Layer 5 — IRQ delivery**. Every other failed row (4.2, 4.3, 6.4, 7.1)
is downstream of "no card IRQ ever fires".

Specifically: in the 200,000-entry IRQ ring (full SIO_IRQ_RING_CAP),
**zero entries have source = `card`**. 542 transactions were attempted,
each rearmed `sio_irq_countdown = SIO_IRQ_DELAY_CARD = 8` on the TX
write, and zero of those countdowns ever reached 0.

### Why the existing access-paced model can't deliver card IRQs

`sio_tick()` decrements the countdown on each call. It is called from
(`grep -rn 'sio_tick' runtime/`):

- every `sio_read()` (sio.c:830) — SIO_RX_DATA / SIO_STAT / etc.
- every I_STAT read (memory.c:220, 309)
- every read of an `0x1F801070..0x1F801074` register (memory.c:365)
- every interpreter step (psx_interpreter.c:517)

For pads (delay = 4), the BIOS pad-poll routine touches SIO heavily —
it tight-polls SIO_STAT after each TX byte until RX_RDY appears.
4 ticks accumulate easily, IRQ fires.

For cards (delay = 8), the BIOS card-protocol path is **chain-driven,
not poll-driven**: the chain handler writes one TX byte and returns,
expecting the next IRQ to drive the next byte. Between TX writes the
BIOS performs only a handful of SIO accesses. The countdown does not
reach 0 before the next TX rearms it. The IRQ never fires, so the
chain never re-enters, so the next byte never goes out, so the BIOS
times out and writes SIO_CTRL.RESET — which `txn_close` records as
`abort_reselect`.

This is not an unmasked-IRQ issue (Transcript C confirms I_MASK bit 7
is set on every chain enter). It is not a state-machine-byte issue
(Transcript A confirms the bytes our state machine returns are correct
for the first 4 positions). It is a pacing-model issue.

---

## Proposed fix (scoped to Layer 5)

The fix must move from access-paced to **time/cycle-paced** SIO IRQ
delivery, which is what real hardware does (~170 cycles after each
ACK, independent of register-access count). The branch name
`feature/cycle-paced-sio-irq` already states this intention; the
implementation has not landed.

### Concrete proposal

1. **Replace `sio_irq_countdown` with a frame-clock deadline.** Add a
   monotonic counter ticked once per dispatched recompiled function
   (or once per VBlank — the runtime does not have a true cycle
   counter). On TX write, set
   `sio_irq_deadline = sio_clock + N` where N is calibrated to "fires
   within the same VBlank for a 138-byte card sector at 60 Hz" → at
   most ~430 µs per byte → calibrate to ~1 dispatch loop iteration for
   pads, similar for cards.
2. **Drive `sio_tick()` from the dispatch loop**, not from register
   accesses. Call `sio_tick()` from `psx_dispatch` once per dispatched
   function (or once per N functions). This gives consistent pacing
   regardless of what the BIOS happens to be doing between bytes.
3. **Keep the access-paced ticks as a faster fast-path** so existing
   pad protocol stays correct (or — preferably — re-validate pad
   timing after the change and consolidate to the single model).

### Risk-bounded interim step

Before doing the full rework, run a **calibration probe**: change
`SIO_IRQ_DELAY_CARD` from 8 → 4 (matching pad). If 4 is enough to let
even one card IRQ through, we will see card-source entries appear in
`sio_irq_dump`, mc_max_state will exceed 4, and `card_txn_dump` will
show a transaction that gets past `terminal_state=4`. If yes — the
diagnosis is correct and we plan the cycle-paced rewrite. If no — the
problem is elsewhere (chain abort flag at [0x755A], install-handler
miswire) and we revisit.

This calibration is exactly the kind of "is the hypothesis true"
measurement the audit was designed to gate. **No code change should
land until the calibration probe demonstrates a measurable shift in
the metrics this audit captured.**

### Out of scope for the first fix

- Layer 1 file-name divergence (card?.mcd vs dummy.?.mcr) — should be
  unified before the next cross-oracle byte-diff session, but does not
  block the native-side card path from working.
- The 23k-line/min `[dirty_ram_interp] WARN` fprintf flood
  (CLAUDE.md §3 violation in `runtime/src/dirty_ram_interp.c`) —
  separate cleanup task; doesn't block this fix.
- Visual sprite corruption on highlighted UI items (cursor sprite
  rendering) — not on the card data path.
- The MEMORY CARD operations menu rendering after no-data — that's
  expected BIOS behavior when the directory read fails. Once Layer 5
  is fixed, we re-test Layer 7.

---

## Verification (after fix lands)

End-to-end success of the fix:

1. `python tools/raw_tcp.py 4370 sio_irq_dump count=200 src=1` — must
   show non-empty entries (≥1 card-source IRQ).
2. `python tools/raw_tcp.py 4370 card_txn_dump count=20` — must show
   ≥1 entry with `terminal_state` reaching `MC_READ_END=12` and
   `end_reason: success`.
3. `python tools/raw_tcp.py 4370 read_ram addr=0x800072F0 len=4` —
   must show `[0x72F0]` reaching values > 1 (confirming the
   `phase4_72f0_one_byte_per_vblank` hypothesis is resolved).
4. `python tools/raw_tcp.py 4370 sio_state` — `mc_read_done` must be
   ≥ 1.
5. `screenshot_file` after navigating into CARD 1 — the BMP must show
   the save title `BISLPS-01234SAVE00` (from `card1.mcd` frame 1) as
   a list entry.

The screenshot (5) is ground truth. Until the save title is on screen
the session is not complete.

---

## Tooling additions / fixes made during this audit

These are minor infrastructure touch-ups, not BIOS-path changes. They
are committed regardless of whether the fix proposal is accepted, so
the audit harness stays repeatable.

- `tools/debug_client.py` — `send_cmd()` now reads multi-line JSON via
  brace/bracket balance instead of stopping at the first `\n`; the
  `pretty_regs` formatter now also reads the server's `gpr` array
  (positional) when no `regs` dict is present. Both were broken
  before, hiding all GPRs and corrupting `card_txn_dump` parsing.
- `tools/raw_tcp.py` — same brace-balance read; `0x…` arg values are
  preserved as JSON strings (server's `read_ram` etc. expect strings).
- `runtime/include/memcard.h` + `runtime/src/memcard.c` — new
  `memcard_debug_read_buffer()` accessor.
- `runtime/src/debug_server.c` — new `card_buffer_dump` TCP command
  (slot, offset, len → hex). Used by `tools/verify_card_buffer.py` to
  prove Layer 2.
- `tools/verify_card_buffer.py` — pulls slot 0/1 buffers and
  sha256-compares to disk.

No edits to generated code (Rule 4 / Rule 14). No fprintf added.

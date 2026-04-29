#!/usr/bin/env python3
"""
diff_sio_traces.py — compare our recompiled BIOS's SIO byte stream against
Beetle PSX's golden reference, looking for the divergence that explains why
our card directory load fails while Beetle's succeeds.

Usage:
    python3 tools/diff_sio_traces.py ours.json beetle.json [--from-byte N]

Both files are JSON dumps from debug-server commands:
    ours    → /sio_trace
    beetle  → /emu_sio_trace

Beetle entries:  {seq, tx, rx, ctrl}
Ours entries:    {seq, tx, rx, ctrl, mc_pre, mc_post, dev_pre, dev_post,
                  func, abort, irq_cd, in_exc, ctr, sr}

The diff focuses on TX/RX byte sequences during card protocol bursts (= runs
that start with 0x81). Pad bytes (0x01) are interleaved in both streams; we
filter them out and compare card-only byte sequences side-by-side.
"""

import json
import sys
from collections import Counter


def hx(s):
    return int(s, 16) if isinstance(s, str) else s


def load(path):
    with open(path) as f:
        return json.loads(f.read())


def find_card_runs(entries):
    """Return list of (start_idx, length) for each contiguous card-protocol
       run (starts with 0x81 TX, ends when 0x01 TX or another 0x81 appears)."""
    runs = []
    i = 0
    while i < len(entries):
        if hx(entries[i]['tx']) == 0x81:
            j = i + 1
            while j < len(entries):
                tx = hx(entries[j]['tx'])
                if tx == 0x01 or tx == 0x81:
                    break
                j += 1
            runs.append((i, j - i))
            i = max(j, i + 1)
        else:
            i += 1
    return runs


def card_only_seq(entries):
    """Extract just the card-protocol bytes (TX, RX) skipping pad polls."""
    seq = []
    in_card = False
    for e in entries:
        tx = hx(e['tx'])
        rx = hx(e['rx'])
        if tx == 0x81:
            in_card = True
        elif tx == 0x01:
            in_card = False
            continue
        if in_card:
            seq.append((tx, rx))
    return seq


def fmt_byte(tx, rx):
    return f"tx=0x{tx:02X} rx=0x{rx:02X}"


def compare(ours_path, beetle_path, from_byte=0):
    ours_doc = load(ours_path)
    beet_doc = load(beetle_path)
    ours = ours_doc.get('entries', [])
    beet = beet_doc.get('entries', [])

    print(f"=== Inputs ===")
    print(f"  ours:   {len(ours):6d} entries (total seq: {ours_doc.get('total','?')})")
    print(f"  beetle: {len(beet):6d} entries (total seq: {beet_doc.get('total','?')})")

    ours_runs = find_card_runs(ours)
    beet_runs = find_card_runs(beet)

    print(f"\n=== Card-protocol runs (= consecutive non-pad bytes after 0x81) ===")
    print(f"  ours:   {len(ours_runs):4d} runs   "
          f"len distribution: {sorted(Counter(r[1] for r in ours_runs).items())}")
    print(f"  beetle: {len(beet_runs):4d} runs   "
          f"len distribution: {sorted(Counter(r[1] for r in beet_runs).items())}")

    # Show longest run from each — this tells us how many bytes get sent in one
    # uninterrupted protocol burst.  If Beetle's longest is ~137 (full read) and
    # ours is ~11, that's the structural smoking gun.
    if ours_runs and beet_runs:
        ours_longest = max(ours_runs, key=lambda r: r[1])
        beet_longest = max(beet_runs, key=lambda r: r[1])
        print(f"\n  ours   longest run: {ours_longest[1]:4d} bytes (idx {ours_longest[0]})")
        print(f"  beetle longest run: {beet_longest[1]:4d} bytes (idx {beet_longest[0]})")

        # Print Beetle's longest run side by side with ours
        ours_run = ours[ours_longest[0]:ours_longest[0]+ours_longest[1]]
        beet_run = beet[beet_longest[0]:beet_longest[0]+beet_longest[1]]
        print(f"\n  --- side-by-side longest runs (ours | beetle) ---")
        n = max(len(ours_run), len(beet_run))
        for i in range(min(n, 50)):
            o = f"{fmt_byte(hx(ours_run[i]['tx']), hx(ours_run[i]['rx']))}" if i < len(ours_run) else "—"
            b = f"{fmt_byte(hx(beet_run[i]['tx']), hx(beet_run[i]['rx']))}" if i < len(beet_run) else "—"
            marker = ""
            if i < len(ours_run) and i < len(beet_run):
                if hx(ours_run[i]['rx']) != hx(beet_run[i]['rx']):
                    marker = "  <-- RX DIFFER"
                elif hx(ours_run[i]['tx']) != hx(beet_run[i]['tx']):
                    marker = "  <-- TX DIFFER"
            print(f"  {i:3d}  {o:32s}  |  {b:32s}{marker}")

    # Card-only sequences (all card bytes concatenated, pad bytes excluded)
    o_card = card_only_seq(ours)
    b_card = card_only_seq(beet)
    print(f"\n=== Card-only flat sequences (pad polls filtered) ===")
    print(f"  ours:   {len(o_card)} card bytes total")
    print(f"  beetle: {len(b_card)} card bytes total")

    # Find first divergence in card-only sequences
    n = min(len(o_card), len(b_card))
    div = None
    for i in range(from_byte, n):
        if o_card[i] != b_card[i]:
            div = i
            break
    if div is None:
        if len(o_card) == len(b_card):
            print(f"  No divergence in first {n} card bytes — sequences match!")
        else:
            print(f"  No byte mismatch in first {n} bytes; one stream is shorter")
    else:
        print(f"\n  FIRST DIVERGENCE at card-byte index {div} (out of {n} comparable):")
        start = max(0, div - 5)
        end = min(n, div + 10)
        for i in range(start, end):
            o_tx, o_rx = o_card[i]
            b_tx, b_rx = b_card[i]
            marker = "  <-- DIFFER" if (o_tx, o_rx) != (b_tx, b_rx) else ""
            print(f"    {i:4d}  ours: tx=0x{o_tx:02X} rx=0x{o_rx:02X}  | beetle: tx=0x{b_tx:02X} rx=0x{b_rx:02X}{marker}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: diff_sio_traces.py ours.json beetle.json [--from-byte N]")
        sys.exit(1)
    from_byte = 0
    if "--from-byte" in sys.argv:
        i = sys.argv.index("--from-byte")
        from_byte = int(sys.argv[i+1])
    compare(sys.argv[1], sys.argv[2], from_byte)

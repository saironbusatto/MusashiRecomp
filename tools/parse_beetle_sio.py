#!/usr/bin/env python3
"""Parse Beetle PSX SIO trace and show card transactions."""
import json
import sys

path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/beetle_sio_trace.json'
with open(path) as f:
    data = json.load(f)

print(f"Total: {data['total']}, Count: {data['count']}")

in_card = False
card_bytes = []
card_count = 0
pad_count = 0

for e in data['entries']:
    tx = int(e['tx'], 16) if isinstance(e['tx'], str) else e['tx']
    rx = int(e['rx'], 16) if isinstance(e['rx'], str) else e['rx']
    ctrl = int(e['ctrl'], 16) if isinstance(e['ctrl'], str) else e['ctrl']

    if tx == 0x81:
        if card_bytes:
            print(f"  [{len(card_bytes)} bytes total]")
        in_card = True
        card_bytes = []
        card_count += 1
        print(f"\n--- CARD TRANSACTION #{card_count} (seq={e['seq']}) ctrl=0x{ctrl:04X} ---")

    if in_card:
        card_bytes.append((tx, rx))
        print(f"  [{len(card_bytes):3d}] tx=0x{tx:02X} rx=0x{rx:02X} ctrl=0x{ctrl:04X}")
    else:
        if tx == 0x01:
            pad_count += 1

    # End of card transaction: when tx is 0x01 (pad) after card bytes
    if in_card and tx == 0x01 and len(card_bytes) > 1:
        in_card = False
        # Remove the pad byte we just added
        card_bytes.pop()
        print(f"  [{len(card_bytes)} card bytes, then pad 0x01]")

if card_bytes and in_card:
    print(f"  [{len(card_bytes)} bytes, transaction still in progress]")

print(f"\nSummary: {card_count} card transactions, {pad_count} pad polls (not in card)")

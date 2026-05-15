"""Extract the card-data byte stream the BIOS received during OPTIONS-black.

The kernel SIO byte ISR at 0xBFC16024 emits `sb v0,0x0(k0)` — every byte
the card delivers gets stored at k0 (which increments per byte). Filter
wtrace_all by that PC, sort by seq, and reconstruct the byte stream.
Compare to tomba.0.mcr / tomba.1.mcr.
"""
import json, os, sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'
mcr_root = 'F:/Projects/TombaRecomp/saves'

with open(path) as f:
    d = json.load(f)

# Filter by the kernel-SIO-handler byte-store PC. The handler lives at
# BIOS 0xBFC16024, but the recompiler runs the RAM mirror at 0x00006524
# (= 0xBFC16024 - 0xBFC10000 + 0x500). Either could show up depending
# on whether code came from ROM (BIOS path) or RAM-mirror (kernel).
STORE_PCS = ('0xBFC16024', '0x00006524', '0x06524')

hits = [e for e in d['wtrace_all']
        if e['pc'].upper() in (p.upper() for p in STORE_PCS)
        and e.get('w', 1) == 1]
print(f'BIOS card-byte stores: {len(hits)} hits')
if not hits:
    sys.exit(1)

hits.sort(key=lambda e: int(e['seq']))

# Group by contiguous bursts. Each card command reads N bytes consecutively
# into a buffer; bursts are separated by gaps in seq (handler not running).
bursts = []
cur = []
for i, e in enumerate(hits):
    if cur and int(e['seq']) > int(cur[-1]['seq']) + 5000:
        bursts.append(cur)
        cur = []
    cur.append(e)
if cur:
    bursts.append(cur)
print(f'Bursts: {len(bursts)}')
for i, b in enumerate(bursts):
    addrs = [int(e['addr'], 16) for e in b]
    print(f'  Burst {i}: {len(b)} bytes, addr range '
          f'{hex(min(addrs))}..{hex(max(addrs))}, seq '
          f'{b[0]["seq"]}..{b[-1]["seq"]}')

# Examine the most-recent (likely complete) burst.
b = bursts[-1] if bursts else []
print(f'\n=== Last burst ({len(b)} bytes) ===')
print('First 32 entries:')
for e in b[:32]:
    print(f'  seq={e["seq"]} addr={e["addr"]} val=0x{int(e["new"], 16) & 0xFF:02X} frame={e["frame"]}')
print('...')
print('Last 16 entries:')
for e in b[-16:]:
    print(f'  seq={e["seq"]} addr={e["addr"]} val=0x{int(e["new"], 16) & 0xFF:02X} frame={e["frame"]}')

# Bytes in the order they were stored.
recv = bytes(int(e['new'], 16) & 0xFF for e in b)
print(f'\nReceived stream hex (first 256 bytes):')
for i in range(0, min(256, len(recv)), 32):
    print(f'  {i:04x}: ' + ' '.join(f'{b:02x}' for b in recv[i:i+32]))

# Compute the BIOS-style XOR checksum over the first 128 bytes.
if len(recv) >= 129:
    xor = 0
    for byte in recv[:128]:
        xor ^= byte
    print(f'\nXOR of first 128 bytes: 0x{xor:02X}')
    print(f'Byte at offset 128 (claimed checksum): 0x{recv[128]:02X}')
    print(f'Match: {xor == recv[128]}')

# Compare to the actual save files.
for fp in ('F:/Projects/TombaRecomp/saves/card1.mcd',
           'F:/Projects/TombaRecomp/saves/card2.mcd'):
    fname = os.path.basename(fp)
    if not os.path.exists(fp):
        print(f'\n{fname}: not found')
        continue
    with open(fp, 'rb') as fh:
        mcr = fh.read()
    print(f'\n{fname}: {len(mcr)} bytes')
    print('First 256 bytes:')
    for i in range(0, 256, 32):
        print(f'  {i:04x}: ' + ' '.join(f'{b:02x}' for b in mcr[i:i+32]))
    # See if our recv stream matches any window of the mcr.
    if len(recv) > 32:
        needle = recv[:32]
        idx = mcr.find(needle)
        print(f'  Match of first 32 received bytes anywhere in {fname}: '
              f'{hex(idx) if idx >= 0 else "NO"}')

"""Properly split the SIO byte stream into sector-sized groups.

Each sector read fills the buffer at 0xBE48..0xBEC7 (128 bytes). k0 resets
when a new sector starts. Group by addr-decrease, then compare each
captured sector against the real card1.mcd content.
"""
import json, os, sys

path = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'

with open(path) as f:
    d = json.load(f)

STORE_PCS = {'0xBFC16024', '0x00006524'}
hits = [e for e in d['wtrace_all']
        if e['pc'].upper() in {p.upper() for p in STORE_PCS}
        and e.get('w', 1) == 1]
hits.sort(key=lambda e: int(e['seq']))
print(f'{len(hits)} byte-store events captured')

# Group by sector: a new sector starts when the destination address
# decreases (or, more reliably, when it equals the lowest seen base).
addrs = [int(e['addr'], 16) for e in hits]
base = min(addrs)
print(f'Buffer base: 0x{base:04X}')

sectors = []
cur = []
prev_addr = -1
for e in hits:
    a = int(e['addr'], 16)
    if a < prev_addr or (a == base and cur):
        sectors.append(cur)
        cur = []
    cur.append(e)
    prev_addr = a
if cur:
    sectors.append(cur)

print(f'Sectors identified: {len(sectors)}')
for i, s in enumerate(sectors):
    addrs = [int(e['addr'], 16) for e in s]
    print(f'  Sector {i}: {len(s)} bytes, addr range 0x{min(addrs):04X}..0x{max(addrs):04X}, '
          f'frames {s[0]["frame"]}..{s[-1]["frame"]}')

def sector_bytes(s):
    """Reconstruct sector buffer from the stores (last write wins per addr)."""
    buf = {}
    for e in s:
        buf[int(e['addr'], 16)] = int(e['new'], 16) & 0xFF
    base = min(buf)
    return bytes(buf[base + i] if (base + i) in buf else 0
                 for i in range(max(buf) - base + 1))

# Pull real card data for comparison.
with open('F:/Projects/TombaRecomp/saves/card1.mcd', 'rb') as fh:
    card1 = fh.read()

# A PS1 memcard read typically starts at the directory (sector 0), then
# reads sectors 1..15. Each "frame" in the .mcd is 128 bytes.
for i, s in enumerate(sectors):
    recv = sector_bytes(s)
    print(f'\n=== Sector {i}: {len(recv)} bytes ===')
    print('Received hex (full):')
    for off in range(0, len(recv), 32):
        print(f'  {off:03x}: ' + ' '.join(f'{b:02x}' for b in recv[off:off+32]))

    # Try to find a window in card1.mcd that this matches.
    if len(recv) >= 8:
        for fr in range(0, 16):
            real = card1[fr*128:(fr+1)*128]
            if recv[:len(recv)] == real[:len(recv)]:
                print(f'  EXACT match: card1.mcd frame {fr}')
                break
            # Try matching the prefix
            common = 0
            for j in range(min(len(recv), len(real))):
                if recv[j] == real[j]:
                    common += 1
                else:
                    break
            if common >= 8:
                print(f'  Prefix match with card1.mcd frame {fr}: {common}/{len(recv)} bytes')
                # If mismatch starts mid-stream, show the divergence
                if common < len(recv):
                    div = common
                    print(f'  Divergence at offset 0x{div:02X}:')
                    print(f'    received[{div}..{div+16}]: ' + ' '.join(f'{b:02x}' for b in recv[div:div+16]))
                    print(f'    real    [{div}..{div+16}]: ' + ' '.join(f'{b:02x}' for b in real[div:div+16]))
                    # See if received[div:] matches real[div+1:] (shifted by 1)
                    if div+1 < len(real):
                        shifted = real[div+1:div+1+min(16, len(recv)-div)]
                        match_shift = recv[div:div+len(shifted)] == shifted
                        print(f'    SHIFT-BY-1 match: {match_shift}')
                break

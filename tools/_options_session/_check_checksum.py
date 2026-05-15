"""Compute expected per-frame XOR for card1.mcd frames 0..3 and compare to
the running XOR captured in the dump trace (at 0x7560 after byte 127 store)."""
import json, sys

dump = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'

with open('F:/Projects/TombaRecomp/saves/card1.mcd', 'rb') as fh:
    card = fh.read()

print(f"Card file: {len(card)} bytes ({len(card)//128} frames)")

# Compute XOR over each frame for sectors 0..3
for frame in range(0, 16):
    fbytes = card[frame*128:(frame+1)*128]
    msb = (frame >> 8) & 0xFF
    lsb = frame & 0xFF
    xor_bytes = 0
    for b in fbytes:
        xor_bytes ^= b
    expected = msb ^ lsb ^ xor_bytes
    print(f"  Frame {frame:2d}: MSB={msb:02X} LSB={lsb:02X} "
          f"XOR(bytes 0..127)={xor_bytes:02X}  "
          f"expected_checksum_byte={expected:02X}  "
          f"on-disk-byte-127={fbytes[127]:02X}  "
          f"first_4_bytes={fbytes[0]:02X} {fbytes[1]:02X} {fbytes[2]:02X} {fbytes[3]:02X}")

# Now find each sector's running XOR end value in the dump
with open(dump) as f:
    d = json.load(f)

print("\n\nRunning XOR end-of-sector values from trace (writes to 0x7560 from pc=0xBFC155AC):")
events = d['wtrace_all']
ac_writes = [(i, e) for i, e in enumerate(events)
             if e['pc'].upper() == '0XBFC155AC' and int(e['addr'], 16) == 0x7560]
for i, e in ac_writes:
    val = int(e['new'], 16) & 0xFF
    print(f"  idx={i} seq={e['seq']} fr={e.get('frame')} 0x7560 -> 0x{val:02X}")

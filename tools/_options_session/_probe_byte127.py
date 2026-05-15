"""Diagnose why the 128th SIO-card-data byte never lands in the BIOS buffer.

The BIOS handler at 0xBFC15F08 has gates:
  (1) mem[0x75c0] != 0  (active flag)
  (2) I_STAT bit 0x80 set
  (3) I_MASK bit 0x80 set
  (4) op-byte at *mem[0x75c8] == 2 (READ mode)
On miss, jr ra (no buffer store).

For sector 0 (sees 127 stores 0xBE48..0xBEC6), find:
  - last buffer store seq + frame
  - immediately following writes to 0x75c0 / 0x75c4 / 0x72f0 / I_STAT (1F801070)
  - any unrelated SIO traffic between byte 127 and byte 128 that could
    have flipped a gate.
"""
import json, sys

path = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'

with open(path) as f:
    d = json.load(f)

STORE_PCS_UPPER = {'0xBFC16024', '0x00006524', '0X00006524'}
GATE_ADDRS = {
    0x000075c0: "active_flag",
    0x000075c4: "buffer_ptr",
    0x00007570: "buffer_ptr_alt",   # in case the var is at 0x7570 region
    0x000075c8: "op_byte_ptr",
    0x000075cc: "checksum_ptr",
    0x000072f0: "counter",
    0x1f801070: "I_STAT",
    0x1f801074: "I_MASK",
    0x1f801040: "SIO_DATA",
    0x1f80104a: "SIO_CTRL",
}

events = d['wtrace_all']
print(f"Total wtrace events: {len(events)}")

# Find all buffer stores (the 0xBE48..0xBEC7 range)
buf_stores = []
for i, e in enumerate(events):
    pc = e['pc'].upper()
    if pc in STORE_PCS_UPPER and int(e['addr'], 16) >= 0xBE48 and int(e['addr'], 16) <= 0xBEC7:
        buf_stores.append((i, e))

print(f"Buffer stores 0xBE48..0xBEC7: {len(buf_stores)}")

# Group by sector
sectors = []
cur = []
prev_addr = -1
for i, e in buf_stores:
    a = int(e['addr'], 16)
    if a < prev_addr or (a == 0xBE48 and cur):
        sectors.append(cur)
        cur = []
    cur.append((i, e))
    prev_addr = a
if cur:
    sectors.append(cur)

print(f"Sectors: {len(sectors)}")

# For sector 0, look at events between last byte store and the next
# byte store of sector 1 — that's where the gate-flip must have happened.
for s_idx in range(min(2, len(sectors))):
    s = sectors[s_idx]
    last_i, last_e = s[-1]
    print(f"\n=== Sector {s_idx}: {len(s)} stores ===")
    print(f"  First store: idx {s[0][0]}, addr {s[0][1]['addr']}, frame {s[0][1].get('frame')}")
    print(f"  Last store:  idx {last_i}, addr {last_e['addr']}, frame {last_e.get('frame')}")
    print(f"  Expected next store addr: 0xBEC7 (the 128th byte)")
    print()

    # Show next 60 events after the last byte 127 store, look for any
    # writes to gate addresses, OR any SIO traffic
    print(f"  Events immediately after last store (idx {last_i}):")
    next_buf_idx = sectors[s_idx + 1][0][0] if s_idx + 1 < len(sectors) else last_i + 200
    count = 0
    for j in range(last_i + 1, min(last_i + 200, next_buf_idx)):
        e = events[j]
        addr = int(e['addr'], 16)
        pc = e['pc']
        new = e['new']
        old = e.get('old', '')
        seq = e.get('seq', '?')
        frame = e.get('frame', '?')

        label = GATE_ADDRS.get(addr)
        # Show high-pri gate writes, plus near-buffer writes, plus all SIO writes
        if (label or (0xBE48 <= addr <= 0xBED0)
                or (0x1F801040 <= addr <= 0x1F80104F)
                or (0x1F801070 <= addr <= 0x1F801077)
                or (0x7500 <= addr <= 0x76C0)):
            tag = f"[{label}]" if label else ""
            print(f"    idx={j} seq={seq} fr={frame} pc={pc} addr=0x{addr:08X} {old}->{new} {tag}")
            count += 1
            if count > 50:
                print("    ... (truncated)")
                break

"""Search for any writes to the polled flag region 0xB9D0..0xB9E4."""
import json, sys

path = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'

with open(path) as f:
    d = json.load(f)

LO = 0xB9C0
HI = 0xB9F0

hits = []
for e in d['wtrace_all']:
    a = int(e['addr'], 16)
    if LO <= a < HI:
        hits.append(e)

print(f'Writes in [{hex(LO)}..{hex(HI)}): {len(hits)} of {len(d["wtrace_all"])} total')
for e in hits[:30]:
    print(f'  seq={e["seq"]} addr={e["addr"]} new={e["new"]} pc={e["pc"]} ra={e["ra"]} frame={e["frame"]}')

# Also check broader kernel-data scan to find what IS being written nearby
print()
print('--- broader scan 0xB000..0xC800 ---')
addr_c = {}
for e in d['wtrace_all']:
    a = int(e['addr'], 16)
    if 0xB000 <= a < 0xC800:
        addr_c[e['addr']] = addr_c.get(e['addr'], 0) + 1
print(f'Total in 0xB000..0xC800: {sum(addr_c.values())}')
for a, c in sorted(addr_c.items(), key=lambda x: -x[1])[:20]:
    print(f'  {a}: {c}')

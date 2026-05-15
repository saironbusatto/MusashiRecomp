"""Find what's calling TestEvent / what Tomba is spinning on."""
import json, sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'

with open(path) as f:
    d = json.load(f)

# fn_entry: who calls the Tomba chain handlers (RA tells us the caller)
print('=== fn_entry RA distribution for Tomba functions ===')
for target in ('0x80068024', '0x80068098', '0x80068554'):
    ra_c = Counter(e['ra'] for e in d['fn_entry'] if e['func'] == target)
    print(f'\n{target}:')
    for ra, c in ra_c.most_common(5):
        print(f'  RA {ra}: {c}')

print()
print('=== Top 20 RA addrs across ALL fn_entry ===')
ra_c = Counter(e['ra'] for e in d['fn_entry'])
for ra, c in ra_c.most_common(20):
    print(f'  RA {ra}: {c}')

# Check stores to the EvCB region (typical EvCB starts at 0xE000+)
print()
print('=== Stores in EvCB region (0xE000..0xEC00) ===')
ev_writes = [e for e in d['wtrace_all'] if 0xE000 <= int(e['addr'], 16) < 0xEC00]
print(f'Total EvCB writes: {len(ev_writes)}')
addr_c = Counter(e['addr'] for e in ev_writes)
print('By addr:')
for a, c in addr_c.most_common(15):
    print(f'  {a}: {c}')
# Show a sample of unique values written, to see what modes are being set
val_c = Counter(e['new'] for e in ev_writes)
print('By value:')
for v, c in val_c.most_common(10):
    print(f'  {v}: {c}')

# fn_entry order: see the cycle pattern around the chain handlers
print()
print('=== Last 30 fn_entry events (chronological) ===')
for e in d['fn_entry'][-30:]:
    print(f'  seq={e["seq"]} fn={e["func"]} ra={e["ra"]} a0={e["a0"]} a1={e["a1"]}')

"""Break down fn_entry and wtrace by execution region."""
import json, sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'

with open(path) as f:
    d = json.load(f)

def region_of(addr):
    if addr >= 0xBFC00000:
        return 'BIOS-ROM'
    if addr >= 0x10000:
        return 'TOMBA'
    return 'low-kernel'

fn_c = Counter(e['func'] for e in d['fn_entry'])
print('Top 30 funcs by fn_entry count:')
for f, c in fn_c.most_common(30):
    print(f'  {f}  {c:5}  [{region_of(int(f, 16))}]')

print()
pc_c = Counter(e['pc'] for e in d['wtrace_all'])
buckets = Counter()
for p, c in pc_c.items():
    buckets[region_of(int(p, 16))] += c
print('wtrace_all distribution by store-PC source:')
for r, c in buckets.most_common():
    print(f'  {r:12} {c}')

print()
# Show top Tomba-region PCs (writes from Tomba code)
tomba_pcs = [(p, c) for p, c in pc_c.items() if region_of(int(p, 16)) == 'TOMBA']
tomba_pcs.sort(key=lambda x: -x[1])
print('Top 15 TOMBA-region store PCs:')
for p, c in tomba_pcs[:15]:
    print(f'  {p}  {c}')

# Same for low-kernel
kern_pcs = [(p, c) for p, c in pc_c.items() if region_of(int(p, 16)) == 'low-kernel']
kern_pcs.sort(key=lambda x: -x[1])
print()
print('Top 15 low-kernel-region store PCs:')
for p, c in kern_pcs[:15]:
    print(f'  {p}  {c}')

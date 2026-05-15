"""Look for the outer caller of FUN_bfc08b3c (= 0x00008B3C in recompiled form)."""
import json, sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'

with open(path) as f:
    d = json.load(f)

# Print all distinct func addresses in fn_entry, sorted by occurrence
fn_c = Counter(e['func'] for e in d['fn_entry'])
print(f'Total distinct funcs in fn_entry: {len(fn_c)}')
print()

# Are there any direct entries to 0x00008B3C or 0xBFC08B3C?
candidates = [f for f in fn_c if int(f, 16) in (0x00008B3C, 0xBFC08B3C, 0x80008B3C)]
print(f'Direct entries to FUN_bfc08b3c: {candidates}')

# Any entries to 0x9144 (FUN_bfc09144)?
candidates = [f for f in fn_c if int(f, 16) in (0x00009144, 0xBFC09144)]
print(f'Direct entries to FUN_bfc09144: {[(f, fn_c[f]) for f in candidates]}')

# What about all funcs in 0x8000-0x9999 range (BIOS shell card area)?
print()
print('All BIOS shell area funcs in fn_entry (0x8000-0x9FFF):')
for f, c in sorted(fn_c.items(), key=lambda x: int(x[0], 16)):
    addr = int(f, 16)
    if 0x8000 <= addr < 0xA000:
        print(f'  {f}: {c}')

# What about Tomba code (0x80000000+)?
print()
print('All Tomba code funcs in fn_entry (0x80000000+):')
for f, c in sorted(fn_c.items(), key=lambda x: int(x[0], 16)):
    addr = int(f, 16)
    if 0x80000000 <= addr:
        print(f'  {f}: {c}')

# What RA values come back from Tomba functions?
print()
print('Distinct RA values for Tomba-function entries:')
for target in ('0x80068024', '0x80068098', '0x80068554'):
    print(f'  {target}:')
    ra_c = Counter(e['ra'] for e in d['fn_entry'] if e['func'] == target)
    for ra, c in ra_c.most_common(5):
        print(f'    RA={ra}: {c}')

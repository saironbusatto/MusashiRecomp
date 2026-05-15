"""Inspect thread_trace and look for fiber switches near the freeze."""
import json, sys, collections
dump = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778822828.json'
with open(dump) as f:
    d = json.load(f)

tt = d.get('thread_trace', [])
print(f"thread_trace: {len(tt)} entries")
if tt:
    print(f"  first: {tt[0]}")
    print(f"  last : {tt[-1]}")
    kinds = collections.Counter(e.get('kind') for e in tt)
    print(f"  kind distribution: {kinds}")
    print()
    print(f"Last 30 thread_trace entries:")
    for e in tt[-30:]:
        print(f"  {e}")

# Count 0x650 dispatches (SwitchThread)
db = d.get('dirty_block', [])
print()
print(f"=== 0x650 (SwitchThread) calls in dirty_block ring ===")
sw = [e for e in db if e['target'] == '0x00000650']
print(f"  count: {len(sw)}")
for e in sw[-15:]:
    print(f"  seq={e['seq']} fr={e['frame']} target=0x650 ra={e['ra']} "
          f"a0={e['a0']} a1={e['a1']} sp={e['sp']}")

# Look in fn_entry for 0x650 to count and see all params
fe = d.get('fn_entry', [])
print()
sw2 = [e for e in fe if e['func'] == '0x00000650']
print(f"=== 0x650 calls in fn_entry: {len(sw2)} ===")
for e in sw2[-15:]:
    print(f"  seq={e['seq']} fr={e['frame']} ra={e['ra']} a0={e['a0']} a1={e['a1']}")

# Look at any function in fn_entry with a0=3 (likely ChangeThread syscalls)
a0_3 = [e for e in fe if e.get('a0') == '0x00000003']
print()
print(f"=== fn_entry with a0=0x3 (ChangeThread param) ===")
print(f"  count: {len(a0_3)}")
for e in a0_3[-15:]:
    print(f"  seq={e['seq']} fr={e['frame']} func={e['func']} ra={e['ra']} a1={e['a1']}")

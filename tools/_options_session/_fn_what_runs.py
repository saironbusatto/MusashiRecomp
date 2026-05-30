"""Arm fntrace, sample, find what Tomba functions actually run during black screen."""
import sys, time, collections, json
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

# Make sure fntrace is armed for the entire Tomba RAM range
_dbg.call(4470, "fntrace_arm", target="0x80000000", timeout=2)

time.sleep(2)

d = _dbg.call(4470, "fntrace_dump", count=500, timeout=4)
es = d.get('entries', [])
print(f"Got {len(es)} entries (armed: {d.get('armed')})")

funcs = collections.Counter(e.get('target') for e in es)
print(f"\nTop 20 hottest functions:")
for f, c in funcs.most_common(20):
    print(f"  {f}: {c} hits")

# RA distribution per top target
print(f"\nDecomposition of top 5 functions by caller:")
for f, _ in funcs.most_common(5):
    callers = collections.Counter(e['ra'] for e in es if e['target'] == f)
    print(f"  {f}:")
    for ra, c in callers.most_common(5):
        print(f"    ra={ra}: {c}")

print(f"\nLast 25 entries (newest first):")
for e in es[:25]:
    print(f"  fr={e.get('frame')} target={e['target']} ra={e['ra']} a0={e.get('a0')} a1={e.get('a1')}")

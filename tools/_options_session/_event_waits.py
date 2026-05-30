"""Sample fntrace and identify the event IDs Tomba is polling each frame."""
import sys, time, collections, json
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

_dbg.call(4470, "fntrace_arm", target="0xFFFFFFFF", timeout=2)
time.sleep(1)

d = _dbg.call(4470, "fntrace_dump", count=300, timeout=4)
es = d.get('entries', [])
print(f"Got {len(es)} entries")

# The B0 trampoline is at 0xB0; calls there carry t1=B0 index, a0/a1 = args.
# Filter B0 calls and TestEvent-style entries.
b0 = [e for e in es if e['target'] == '0x000000B0']
print(f"\nB0 trampoline calls: {len(b0)}")
# Group by (ra, t1)
sigs = collections.Counter((e['ra'], e.get('t1','?')) for e in b0)
print(f"Top (ra, t1=B0_idx) signatures:")
for (ra, t1), c in sigs.most_common(10):
    print(f"  ra={ra} t1={t1}: {c}")

# Find every distinct (ra, a0, a1, t1) for B0 calls
sigs2 = collections.Counter((e['ra'], e.get('a0'), e.get('a1'), e.get('t1','?')) for e in b0)
print(f"\nTop (ra, a0, a1, t1) signatures:")
for (ra, a0, a1, t1), c in sigs2.most_common(15):
    print(f"  ra={ra} t1={t1} a0={a0} a1={a1}: {c}")

# Now look at non-B0 calls — what's Tomba running?
non_b0 = [e for e in es if e['target'] != '0x000000B0']
tomba_funcs = collections.Counter(e['target'] for e in non_b0 if e['target'].startswith('0x800'))
print(f"\nTomba (0x800...) functions called:")
for f, c in tomba_funcs.most_common(15):
    callers = collections.Counter(e['ra'] for e in non_b0 if e['target'] == f)
    top_ra = callers.most_common(3)
    print(f"  {f}: {c} hits, top callers: {top_ra}")

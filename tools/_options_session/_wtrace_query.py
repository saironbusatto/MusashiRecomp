import sys, json, time
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

# wtrace_dump returns ALL captured events for the armed ranges
d = _dbg.call(4470, "wtrace_dump", count=100, timeout=4)
es = d.get('entries', [])
print(f"wtrace_dump: {len(es)} entries")
for e in es[:20]:
    print(f"  seq={e.get('seq')} fr={e.get('frame')} pc={e.get('pc')} addr={e.get('addr')} ->{e.get('new')} func={e.get('func','?')} cpu_pc={e.get('cpu_pc','?')}")

# Also look at wtrace_all filtered by addr (the always-on ring)
d2 = _dbg.call(4470, "wtrace_all_dump", count=100, timeout=4) if False else None

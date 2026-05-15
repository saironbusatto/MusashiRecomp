import json, sys
dump = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778822828.json'
with open(dump) as f:
    d = json.load(f)
fe = d['fn_entry']
print(f'fn_entry count: {len(fe)}')
print(f'Last 40 entries:')
for e in fe[-40:]:
    print(f'  seq={e["seq"]} fr={e["frame"]} func={e["func"]} ra={e["ra"]} '
          f'a0={e.get("a0","?")} a1={e.get("a1","?")} t1={e.get("t1","?")}')

print()
print(f'Last 30 wtrace_all entries:')
for e in d['wtrace_all'][-30:]:
    print(f'  seq={e["seq"]} fr={e["frame"]} pc={e["pc"]} addr={e["addr"]} ->{e.get("new","?")}')

print()
print(f'Last 20 restore_trace entries:')
for e in d['restore_trace'][-20:]:
    print(f'  seq={e["seq"]} fr={e["frame"]} kind={e["kind"]} name={e.get("name","?")} '
          f'target={e["target"]} cpu_pc={e["cpu_pc"]} epc={e["epc"]} in_exc={e["in_exc"]}')

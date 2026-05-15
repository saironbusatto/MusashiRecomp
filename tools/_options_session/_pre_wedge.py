"""Find what was happening right before the exception-reentry wedge fired.
The dump captured the wedge — exc_re spikes in heartbeat ticks 62-63.
Look at the LAST wtrace_all entries (frame 763) and the final restore_trace
events to identify the trigger."""
import json, collections

dump = 'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'
with open(dump) as f:
    d = json.load(f)

events = d['wtrace_all']
print(f"=== Last 60 wtrace_all entries (frame near 763) ===")
for e in events[-60:]:
    pc = e['pc']
    addr = int(e['addr'], 16)
    new = e.get('new', '?')
    seq = e['seq']
    fr  = e['frame']
    print(f"  seq={seq:,} fr={fr} pc={pc} addr=0x{addr:08X} ->{new}")

print()
print("=== Last 30 restore_trace events ===")
rt = d['restore_trace']
for e in rt[-30:]:
    print(f"  seq={e.get('seq')} fr={e['frame']} kind={e['kind']} name={e.get('name')} "
          f"jmp={e.get('jmp')} target={e.get('target')} cpu_pc={e.get('cpu_pc')} "
          f"sr={e.get('sr')} epc={e.get('epc')} istat={e.get('istat')} imask={e.get('imask')} "
          f"in_exc={e.get('in_exc')}")

print()
print("=== Last 30 sio_pc_trace events ===")
for e in d['sio_pc_trace'][-30:]:
    print(f"  seq={e['seq']} pc={e['pc']} func={e.get('func')} addr={e['addr']} "
          f"value={e.get('value')} byte_seq={e.get('byte_seq')}")

print()
print("=== Last 20 fn_entry events ===")
for e in d['fn_entry'][-20:]:
    print(f"  seq={e['seq']} fr={e['frame']} func={e['func']} ra={e['ra']} "
          f"a0={e.get('a0','?')} a1={e.get('a1','?')}")

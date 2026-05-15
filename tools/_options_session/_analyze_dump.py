"""Quick analysis of a freeze auto-dump file."""
import json, sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'

with open(path) as f:
    d = json.load(f)

print(f'== Dump: {path}')
print(f'   wall_clock_epoch={d["wall_clock_epoch"]}')
print(f'   wedge={d["wedge_kind_name"]} frame={d["frame_count"]} '
      f'exc_re={d["exception_reentry_blocks"]} '
      f'dispatch_count={d["dispatch_count"]}')
print(f'   in_exc={d["in_exception"]} cur_fn={d["current_func"]} '
      f'last_store_pc={d["last_store_pc"]}')
print(f'   i_stat={d["i_stat"]} i_mask={d["i_mask"]} '
      f'sio_card_active={d["sio_card_active"]} mc_max={d["mc_max_state"]}')

print()
print(f'== wtrace_all: {len(d["wtrace_all"])} entries')
addr_c = Counter(e['addr'] for e in d['wtrace_all'])
pc_c = Counter(e['pc'] for e in d['wtrace_all'])
print('Top 15 ADDRS:')
for a, c in addr_c.most_common(15):
    print(f'  {a}: {c}')
print('Top 15 PCs:')
for p, c in pc_c.most_common(15):
    print(f'  {p}: {c}')

print()
print(f'== fn_entry: {len(d["fn_entry"])} entries')
fn_c = Counter(e['func'] for e in d['fn_entry'])
print('Top 10 funcs entered:')
for f_, c in fn_c.most_common(10):
    print(f'  {f_}: {c}')

print()
print(f'== restore_trace: {len(d["restore_trace"])} entries')
if d['restore_trace']:
    kind_c = Counter(e['name'] for e in d['restore_trace'])
    print('Kinds:', dict(kind_c))
    pc_c2 = Counter(e['cpu_pc'] for e in d['restore_trace'])
    print('Top 10 cpu_pcs:')
    for p, c in pc_c2.most_common(10):
        print(f'  {p}: {c}')

print()
print(f'== thread_trace: {len(d["thread_trace"])} entries')
if d['thread_trace']:
    kind_c = Counter(e['name'] for e in d['thread_trace'])
    print('Kinds:', dict(kind_c))

print()
print(f'== sio_pc_trace: {len(d["sio_pc_trace"])} entries')
if d['sio_pc_trace']:
    addr_c = Counter(e['addr'] for e in d['sio_pc_trace'])
    print('Top 5 SIO addrs:')
    for a, c in addr_c.most_common(5):
        print(f'  {a}: {c}')

print()
print(f'== frame_history: {len(d["frame_history"])} frames')
if d['frame_history']:
    first = d['frame_history'][0]
    last = d['frame_history'][-1]
    print(f'  first frame {first["frame"]}: sr={first["sr"]} i_stat={first["i_stat"]} pad={first["pad"]} disp={first["disp"]}')
    print(f'  last  frame {last["frame"]}: sr={last["sr"]} i_stat={last["i_stat"]} pad={last["pad"]} disp={last["disp"]}')
    # see if display went off
    off_count = sum(1 for f in d['frame_history'] if f['disp']['off'])
    print(f'  display_off frames: {off_count}/{len(d["frame_history"])}')

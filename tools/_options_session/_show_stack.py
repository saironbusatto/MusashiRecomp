"""Show main_stack from a freeze dump."""
import json, sys
dump = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778826348.json'
with open(dump) as f:
    d = json.load(f)

print(f"=== {d['backend']} @ wall={d['wall_clock_epoch']} ===")
print(f"  frame_count: {d['frame_count']}, psx_cyc: {d['psx_cycle_count']:,}")
print(f"  wedge_kind: {d['wedge_kind']} ({d['wedge_kind_name']})")
print(f"  in_exception: {d['in_exception']}, cur_fn: {d['current_func']}, last_store_pc: {d['last_store_pc']}")
print(f"  exc_entries: {d['exception_entries']:,}, exc_reentry: {d['exception_reentry_blocks']:,}")
print(f"  i_stat: {d['i_stat']}, i_mask: {d['i_mask']}")
print(f"  sio_stat: {d['sio_stat']}, sio_ctrl: {d['sio_ctrl']}, sio_card_active: {d['sio_card_active']}, mc_max_state: {d['mc_max_state']}")
print()

ms = d.get('main_stack', [])
print(f"=== main_stack ({len(ms)} frames) ===")
for f in ms:
    s = f.get('symbol', '???')
    o = f.get('displacement', '?')
    m = f.get('module', '?')
    print(f"  [{f['depth']:>3}] {f['addr']}  {m}!{s}+0x{int(o):X}" if o != '?' else f"  [{f['depth']:>3}] {f['addr']}  {m}!{s}")

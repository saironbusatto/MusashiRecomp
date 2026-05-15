"""Analyze the new dirty_block ring from the freeze dump.
What interpreted RAM PCs dominate during the reentry storm?
"""
import json, sys, collections

dump = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778822828.json'

with open(dump) as f:
    d = json.load(f)

print(f"=== Core ===")
print(f"  backend:                 {d['backend']}")
print(f"  wall_clock_epoch:        {d['wall_clock_epoch']}")
print(f"  frame_count:             {d['frame_count']}")
print(f"  psx_cycle_count:         {d['psx_cycle_count']:,}")
print(f"  in_exception:            {d['in_exception']}")
print(f"  current_func:            {d['current_func']}")
print(f"  last_store_pc:           {d['last_store_pc']}")
print(f"  exc_entries:             {d['exception_entries']:,}")
print(f"  exc_reentry_blocks:      {d['exception_reentry_blocks']:,}")
print(f"  dispatch_count:          {d['dispatch_count']:,}")
print(f"  dirty_ram_blocks:        {d['dirty_ram_blocks']:,}")
print(f"  dirty_ram_insns:         {d['dirty_ram_insns']:,}")
print(f"  wedge_kind:              {d.get('wedge_kind','?')} ({d.get('wedge_kind_name','?')})")
print(f"  i_stat:                  {d['i_stat']}")
print(f"  i_mask:                  {d['i_mask']}")
print(f"  mc_max_state:            {d['mc_max_state']}")
print(f"  sio_card_active:         {d['sio_card_active']}")
print(f"  sio_stat / sio_ctrl:     {d['sio_stat']} / {d['sio_ctrl']}")
print(f"  caps:                    {d.get('caps','?')}")

db = d.get('dirty_block', [])
print(f"\n=== dirty_block ring: {len(db)} entries ===")
if not db:
    print("  EMPTY — interpreter never ran (or accessor failed)")
    sys.exit(0)

# Spread by frame
frames = collections.Counter(e['frame'] for e in db)
print(f"  frame range: {min(frames)}..{max(frames)} ({len(frames)} distinct)")
print(f"  top frames: " + ", ".join(f"fr{f}={c}" for f,c in frames.most_common(8)))

# Most frequent target
target_count = collections.Counter(e['target'] for e in db)
print(f"\n=== top dirty-block targets (the chain handlers being hammered) ===")
for tgt, cnt in target_count.most_common(20):
    print(f"  target={tgt}: {cnt} entries")

# Top RA (caller)
ra_count = collections.Counter(e['ra'] for e in db)
print(f"\n=== top RA values (who called these blocks) ===")
for ra, cnt in ra_count.most_common(15):
    print(f"  ra={ra}: {cnt} entries")

# What are the LAST N entries — show full
print(f"\n=== Last 30 dirty_block entries (newest) ===")
for e in db[-30:]:
    print(f"  seq={e['seq']:>10} fr={e['frame']:>3} target={e['target']} ra={e['ra']} "
          f"a0={e.get('a0')} sp={e.get('sp')}")

# Look for "tight loops": pairs (target, ra) that repeat heavily
pair_count = collections.Counter((e['target'], e['ra']) for e in db)
print(f"\n=== Top (target, ra) pairs — exact tight-loop signature ===")
for (t, r), cnt in pair_count.most_common(15):
    print(f"  target={t} ra={r}: {cnt} entries")

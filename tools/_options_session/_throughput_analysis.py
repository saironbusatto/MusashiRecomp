"""Compute per-byte cycle cost and per-frame breakdown from the freeze dump."""
import json, sys, collections

dump = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json'

with open(dump) as f:
    d = json.load(f)

print("=== Core state at dump time ===")
print(f"  frame_count: {d['frame_count']}")
print(f"  psx_cycle_count: {d['psx_cycle_count']:,}")
print(f"  exc_reentry_blocks: {d['exception_reentry_blocks']:,}")
print(f"  exc_entries: {d['exception_entries']:,}")
print(f"  dispatch_count: {d['dispatch_count']:,}")
print(f"  dirty_ram_blocks: {d['dirty_ram_blocks']:,}")
print(f"  dirty_ram_insns: {d['dirty_ram_insns']:,}")
print()

# Heartbeat ring: psx_cycle_count over time
hb = d['heartbeat_ring']
print(f"=== Heartbeat ring ({len(hb)} entries) ===")
if len(hb) >= 2:
    # entries are time-ordered, oldest first based on 'wall'
    hb_sorted = sorted(hb, key=lambda e: e['wall'])
    first, last = hb_sorted[0], hb_sorted[-1]
    cyc_delta = last['cyc'] - first['cyc']
    fr_delta  = last['frame'] - first['frame']
    exc_re_delta = last['exc_re'] - first['exc_re']
    dirty_delta = last['dirty_insns'] - first['dirty_insns']
    wall_delta = last['wall'] - first['wall']  # seconds (integer)
    print(f"  wall delta: {wall_delta} sec")
    print(f"  cycle delta: {cyc_delta:,} ({cyc_delta/33868800:.2f}s of PSX time)")
    print(f"  frame delta: {fr_delta}")
    print(f"  exc_re delta: {exc_re_delta:,}")
    print(f"  dirty_insns delta: {dirty_delta:,}")
    print()
    if wall_delta:
        print(f"  frames/sec (wall): {fr_delta/wall_delta:.1f}")
        print(f"  PSX cycles/sec (wall): {cyc_delta/wall_delta:,.0f}  (real PSX = 33,868,800)")
        print(f"  speed pct: {(cyc_delta/wall_delta)/33868800*100:.1f}%")
        print(f"  dirty_insns/sec: {dirty_delta/wall_delta:,.0f}")
        print(f"  exc_re/sec: {exc_re_delta/wall_delta:,.0f}")

# Count buffer stores per frame from wtrace
events = d['wtrace_all']
# Newest events; estimate window
seqs = [e['seq'] for e in events]
print()
print(f"=== wtrace_all window ===")
print(f"  seq range: {min(seqs):,} .. {max(seqs):,}")

frames_seen = sorted({e['frame'] for e in events})
print(f"  frame range: {min(frames_seen)} .. {max(frames_seen)}  ({len(frames_seen)} distinct)")

# stores per frame
STORE_PCS_UPPER = {'0X00006524', '0XBFC15590'}
stores_per_frame = collections.Counter()
total_buffer_stores = 0
for e in events:
    pc = e['pc'].upper()
    if pc in STORE_PCS_UPPER and 0xBE48 <= int(e['addr'], 16) <= 0xBEC7:
        stores_per_frame[e['frame']] += 1
        total_buffer_stores += 1
print(f"  card-buffer stores total: {total_buffer_stores}")
print(f"  per frame:")
for fr in sorted(stores_per_frame):
    print(f"    fr {fr}: {stores_per_frame[fr]} stores")

# How many bytes did SIO process? Look at sio_pc_trace for tx_data writes (1F801040)
sio_pc = d['sio_pc_trace']
tx_writes = [e for e in sio_pc if int(e['addr'], 16) == 0x1F801040]
print()
print(f"=== SIO TX activity ===")
print(f"  total sio_pc_trace entries: {len(sio_pc)}")
print(f"  TX writes (to 0x1F801040): {len(tx_writes)}")
tx_seqs = [e['seq'] for e in tx_writes]
if tx_seqs:
    seq_window = max(tx_seqs) - min(tx_seqs)
    print(f"  TX byte_seq range: {min(tx_seqs):,} .. {max(tx_seqs):,} (span {seq_window:,})")

# What's the largest cycle gap between successive heartbeat entries?
print()
print(f"=== Heartbeat: per-tick analysis ===")
for i in range(1, min(8, len(hb_sorted))):
    a, b = hb_sorted[i-1], hb_sorted[i]
    cd = b['cyc'] - a['cyc']
    fd = b['frame'] - a['frame']
    rd = b['exc_re'] - a['exc_re']
    dd = b['dirty_insns'] - a['dirty_insns']
    print(f"  tick {i}: cyc+{cd:,} fr+{fd} exc_re+{rd} dirty+{dd:,}  cur_fn={a['cur_fn']} in_exc={a['in_exc']}")
print("  ...")
for i in range(max(1, len(hb_sorted)-6), len(hb_sorted)):
    a, b = hb_sorted[i-1], hb_sorted[i]
    cd = b['cyc'] - a['cyc']
    fd = b['frame'] - a['frame']
    rd = b['exc_re'] - a['exc_re']
    dd = b['dirty_insns'] - a['dirty_insns']
    print(f"  tick {i}: cyc+{cd:,} fr+{fd} exc_re+{rd} dirty+{dd:,}  cur_fn={a['cur_fn']} in_exc={a['in_exc']}")

# Function entry counts — what's running most
print()
print(f"=== Function entry hotspots ===")
fe = d['fn_entry']
fc = collections.Counter(e['func'] for e in fe)
print(f"  Total fn_entry events: {len(fe)}")
for func, ct in fc.most_common(15):
    print(f"    {func}: {ct} calls")

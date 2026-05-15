"""Print full heartbeat-ring progression to identify the moment frames stopped."""
import json, sys
dump = sys.argv[1] if len(sys.argv) > 1 else \
    'F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778822828.json'
with open(dump) as f:
    d = json.load(f)

hb = sorted(d['heartbeat_ring'], key=lambda e: e['wall'])
print(f"{'idx':>4} {'wall':>11} {'frame':>8} {'cyc':>14} {'fr-d':>5} {'cyc-d':>10} "
      f"{'exc_re-d':>10} {'dirty-d':>10} {'cur_fn':>12} {'in_exc':>2}")
for i, e in enumerate(hb):
    if i == 0:
        print(f"{i:>4} {e['wall']:>11} {e['frame']:>8} {e['cyc']:>14,} "
              f"{'-':>5} {'-':>10} {'-':>10} {'-':>10} {e['cur_fn']:>12} {e['in_exc']:>2}")
    else:
        a = hb[i-1]
        print(f"{i:>4} {e['wall']:>11} {e['frame']:>8} {e['cyc']:>14,} "
              f"{e['frame']-a['frame']:>5} {e['cyc']-a['cyc']:>10,} "
              f"{e['exc_re']-a['exc_re']:>10,} {e['dirty_insns']-a['dirty_insns']:>10,} "
              f"{e['cur_fn']:>12} {e['in_exc']:>2}")

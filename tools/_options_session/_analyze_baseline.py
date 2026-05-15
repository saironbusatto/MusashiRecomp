"""Analyze the captured options_baseline_*.json snapshot.

Goal: characterize the divergence between runtime (stuck in OPTIONS-black,
constantly rebuilding display list) and beetle (stable OPTIONS, only
touching state cells).

For each side, group the wtrace_all entries by (addr, pc, ra) and
report counts. Tells us:
  - which PCs are writing the OPTIONS-area cells on each backend
  - whether the same PCs exist on both (= same code, wrong data) or
    different PCs (= different code paths entirely)
"""
from __future__ import annotations
import sys, os, json, glob
from collections import defaultdict

if len(sys.argv) > 1:
    path = sys.argv[1]
else:
    out_dir = os.path.join(os.environ.get("TEMP", "/tmp"), "optsess")
    matches = sorted(glob.glob(os.path.join(out_dir, "options_baseline_*.json")))
    if not matches:
        print("no snapshot found", file=sys.stderr); sys.exit(1)
    path = matches[-1]
print(f"using {path}")

with open(path) as f:
    snap = json.load(f)


def hx(v):
    if isinstance(v, str): return int(v, 16) if v.lower().startswith("0x") else int(v)
    return v


def analyze_side(side_name, side):
    print(f"\n{'='*70}\n  {side_name.upper()}\n{'='*70}")
    print(f"  frame={side['ping']['frame']}")
    if "freeze_check" in side:
        fc = side["freeze_check"]
        if isinstance(fc, dict) and fc.get("ok"):
            print(f"  freeze: current_func={fc.get('current_func')} "
                  f"last_store_pc={fc.get('last_store_pc')} "
                  f"in_exc={fc.get('in_exception')} "
                  f"exc_reentry={fc.get('exception_reentry_blocks')}")

    # OPTIONS-area cluster (filtered)
    dmp = side["wtrace_all_dump_optstate"]
    entries = dmp.get("entries", [])
    print(f"\n  optstate writes in ring: {len(entries)} (filter 0x80090C00..0x80091000)")
    if not entries: return

    # Group by addr (which cells are written most)
    by_addr = defaultdict(int)
    for e in entries:
        by_addr[e["addr"]] += 1
    top_addr = sorted(by_addr.items(), key=lambda x: -x[1])[:12]
    print(f"\n  top 12 addrs (count of writes):")
    for a, c in top_addr:
        print(f"    {a}  {c}")

    # Group by (pc, ra) — who is writing
    by_pcra = defaultdict(int)
    for e in entries:
        by_pcra[(e["pc"], e["ra"])] += 1
    top_pcra = sorted(by_pcra.items(), key=lambda x: -x[1])[:10]
    print(f"\n  top 10 (pc, ra) (count):")
    for (pc, ra), c in top_pcra:
        print(f"    pc={pc} ra={ra}  {c}")

    # Distinct PCs (writers) and distinct RAs (callers of writers)
    distinct_pcs = sorted({e["pc"] for e in entries})
    distinct_ras = sorted({e["ra"] for e in entries})
    print(f"\n  distinct write PCs: {len(distinct_pcs)}")
    print(f"  distinct write RAs (callers): {len(distinct_ras)}")
    print(f"  first 6 distinct PCs: {distinct_pcs[:6]}")
    print(f"  first 6 distinct RAs: {distinct_ras[:6]}")

    # Frame range covered in this window
    frames = sorted({e["frame"] for e in entries})
    print(f"\n  frame range in ring: [{min(frames)}, {max(frames)}] ({len(frames)} distinct frames)")

    # Unfiltered tail — where IS the rest of the activity?
    tail = side["wtrace_all_dump_tail"]
    tail_entries = tail.get("entries", [])
    if tail_entries:
        # Top 8 addr bins (granularity: 256-byte buckets)
        bucket = defaultdict(int)
        for e in tail_entries:
            a = hx(e["addr"])
            bucket[a & ~0xFF] += 1
        top_bucket = sorted(bucket.items(), key=lambda x: -x[1])[:8]
        print(f"\n  top 8 256-byte buckets in unfiltered tail of {len(tail_entries)} entries:")
        for a, c in top_bucket:
            print(f"    0x{a:08X}-0x{a+0xFF:08X}  {c}")


analyze_side("runtime", snap["runtime"])
analyze_side("beetle",  snap["beetle"])

# Cross-side: are the same PCs writing the same cells on both?
r_pc_set = {e["pc"] for e in snap["runtime"]["wtrace_all_dump_optstate"]["entries"]}
b_pc_set = {e["pc"] for e in snap["beetle"]["wtrace_all_dump_optstate"]["entries"]}
print(f"\n{'='*70}\n  CROSS-SIDE\n{'='*70}")
print(f"  runtime distinct PCs writing optstate: {len(r_pc_set)}")
print(f"  beetle  distinct PCs writing optstate: {len(b_pc_set)}")
print(f"  intersection: {len(r_pc_set & b_pc_set)}")
print(f"  runtime-only PCs (first 10): {sorted(r_pc_set - b_pc_set)[:10]}")
print(f"  beetle-only  PCs (first 10): {sorted(b_pc_set - r_pc_set)[:10]}")

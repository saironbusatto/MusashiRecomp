"""Capture beetle's OPTIONS-screen baseline + runtime's current state.

After this, when the user presses OPTIONS on runtime (with delay) we
can query the SAME windows on runtime and diff against this baseline.
No racing — always-on rings give us retroactive visibility.
"""
from __future__ import annotations
import sys, os, json, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _dbg import call, RUNTIME_PORT, BEETLE_PORT


def safe(port, cmd, **kw):
    try: return call(port, cmd, timeout=20, **kw)
    except Exception as e: return {"error": str(e)}


def capture(port, label):
    out = {"label": label, "port": port, "ts": time.time()}
    p = safe(port, "ping")
    out["ping"] = p
    out["history"] = safe(port, "history")
    out["wtrace_all_stats"] = safe(port, "wtrace_all_stats")

    # Pull the entire always-on ring (just the OPTIONS area writes via filter)
    # Cluster around 0x80090C00..0x80091000 — wider net than first probe.
    out["wtrace_all_dump_optstate"] = safe(port, "wtrace_all_dump",
        addr_lo="0x80090C00", addr_hi="0x80091000", count=2048)

    # Also full last 4K writes (unfiltered) so we can mine for unexpected cells.
    out["wtrace_all_dump_tail"] = safe(port, "wtrace_all_dump", count=4096)

    h = out["history"]
    if h.get("ok"):
        newest = h["newest"]
        start  = max(0, newest - 199)
        out["frame_range_start"] = start
        out["frame_range_end"]   = newest
        out["frame_timeseries"]  = safe(port, "frame_timeseries", start=start, end=newest)
        out["get_frame_newest"]  = safe(port, "get_frame", frame=newest)

    if label == "runtime":
        out["freeze_check"] = safe(port, "freeze_check", window=512)
    return out


print("Capturing OPTIONS baseline (beetle = good state; runtime = whatever it is now)…")
snap = {
    "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
    "purpose": "OPTIONS-screen baseline (beetle), runtime current-state",
    "runtime": capture(RUNTIME_PORT, "runtime"),
    "beetle":  capture(BEETLE_PORT,  "beetle"),
}

out_dir = os.path.join(os.environ.get("TEMP", "/tmp"), "optsess")
os.makedirs(out_dir, exist_ok=True)
path = os.path.join(out_dir, f"options_baseline_{int(time.time())}.json")
with open(path, "w") as f:
    json.dump(snap, f, indent=2)
print(f"wrote {path} ({os.path.getsize(path)} bytes)")

# Summary
for side in ("runtime", "beetle"):
    s = snap[side]
    pg = s["ping"]
    wa = s["wtrace_all_stats"]
    ws = s["wtrace_all_dump_optstate"]
    print(f"\n=== {side} ===")
    print(f"  frame={pg.get('frame')}")
    print(f"  wtrace_all total={wa.get('total')} cap={wa.get('capacity')}")
    print(f"  optstate-range writes in ring: {ws.get('emitted', 0)} of {ws.get('available', 0)} ring entries")
    # Distinct addrs in optstate window
    entries = ws.get("entries", [])
    distinct = sorted({e["addr"] for e in entries})
    print(f"  distinct optstate addrs: {len(distinct)}")
    if distinct:
        print(f"    first 8: {distinct[:8]}")

# Live read of the probe cells we care about
print("\n=== live read of OPTIONS-probe cells ===")
def lr(port, addr):
    r = safe(port, "read_ram", addr=addr, len=4)
    return r.get("hex", "??")
for cell in ("0x80090CAC", "0x80090DA0", "0x80090DB8", "0x80090DA4", "0x80090DAC", "0x80090DB4"):
    rv = lr(RUNTIME_PORT, cell); bv = lr(BEETLE_PORT, cell)
    print(f"  {cell}  runtime={rv}  beetle={bv}  {'DIFF' if rv != bv else 'same'}")

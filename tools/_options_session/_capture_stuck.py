"""Capture the current divergence snapshot between runtime and beetle
when runtime is in a stuck/black-screen state.

Reads from the rings we just built — no arming, no probes:
  - always-on wtrace_all on BOTH (last 200 writes)
  - per-frame ring last 200 frames on BOTH (CPU/display/IRQ over time)
  - freeze_check on runtime (handler/dispatch/dirty diagnostics)

Writes everything to /tmp/optsess/stuck_<timestamp>.json so we don't
keep re-querying as state churns.
"""
from __future__ import annotations
import sys, os, json, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _dbg import call, RUNTIME_PORT, BEETLE_PORT


def safe(port, cmd, **kw):
    try: return call(port, cmd, **kw, timeout=15)
    except Exception as e: return {"error": str(e)}


def capture(port, label):
    out = {"label": label, "port": port}
    out["ping"] = safe(port, "ping")
    out["history"] = safe(port, "history")
    out["wtrace_all_stats"] = safe(port, "wtrace_all_stats")
    out["wtrace_all_dump"] = safe(port, "wtrace_all_dump", count=512)
    # Per-frame ring: pull last 200 frames
    h = out["history"]
    if h.get("ok"):
        newest = h["newest"]
        start = max(0, newest - 199)
        out["frame_range_start"] = start
        out["frame_range_end"]   = newest
        out["frame_timeseries"]  = safe(port, "frame_timeseries", start=start, end=newest)
        out["get_frame_newest"]  = safe(port, "get_frame", frame=newest)
    if label == "runtime":
        out["freeze_check"] = safe(port, "freeze_check", window=512)
        out["wtrace_ranges"] = safe(port, "wtrace_ranges")
    return out


print("Capturing snapshot…")
snap = {
    "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
    "runtime": capture(RUNTIME_PORT, "runtime"),
    "beetle":  capture(BEETLE_PORT,  "beetle"),
}

out_dir = os.path.join(os.environ.get("TEMP", "/tmp"), "optsess")
os.makedirs(out_dir, exist_ok=True)
out_path = os.path.join(out_dir, f"stuck_{int(time.time())}.json")
with open(out_path, "w") as f:
    json.dump(snap, f, indent=2)
print(f"wrote {out_path} ({os.path.getsize(out_path)} bytes)")

# Print a one-screen summary
print(f"\nruntime: frame={snap['runtime']['ping']['frame']} disp_miss={snap['runtime']['ping']['dispatch_miss_total']}")
print(f"beetle : frame={snap['beetle']['ping']['frame']}")
fc = snap["runtime"]["freeze_check"]
if fc.get("ok"):
    print(f"\nruntime freeze_check:")
    for k in ("current_func", "last_store_pc", "i_stat", "i_mask",
             "exception_reentry_blocks", "dirty_ram_blocks", "dispatch_count",
             "frame_count", "in_exception", "sio_stat", "mc_max_state"):
        if k in fc: print(f"  {k:32s} = {fc[k]}")

# Last 5 writes on each backend's catch-all
print(f"\nruntime wtrace_all newest 3:")
for e in snap["runtime"]["wtrace_all_dump"]["entries"][-3:]:
    print(f"  seq={e['seq']} addr={e['addr']} new={e['new']} pc={e['pc']} ra={e['ra']} fr={e['frame']}")
print(f"beetle  wtrace_all newest 3:")
for e in snap["beetle"]["wtrace_all_dump"]["entries"][-3:]:
    print(f"  seq={e['seq']} addr={e['addr']} new={e['new']} pc={e['pc']} ra={e['ra']} fr={e['frame']}")

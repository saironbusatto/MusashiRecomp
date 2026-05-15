"""Smoke-test the normalized wtrace verb set on BOTH backends.

Both backends must accept identical JSON for the same verbs.  This
script issues the same six probes against runtime (4470) and beetle
(4380) and compares response shapes.  Any divergence is a parity bug
in the debug server; fix it before relying on cross-backend tooling.
"""
from __future__ import annotations
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _dbg import call, RUNTIME_PORT, BEETLE_PORT


def probe(name, **kw):
    out = {}
    for label, port in [("runtime", RUNTIME_PORT), ("beetle", BEETLE_PORT)]:
        try:
            out[label] = call(port, name, **kw)
        except Exception as e:
            out[label] = {"error": str(e)}
    return out


def shape(r):
    if not isinstance(r, dict): return type(r).__name__
    return sorted(k for k in r.keys() if k != "id")


print("== wtrace_disarm_all (idempotent reset of armed ranges) ==")
r = probe("wtrace_disarm_all")
print("  runtime:", r["runtime"])
print("  beetle :", r["beetle"])

print("\n== wtrace_arm lo=0x80090C80 hi=0x80090DE0 ==")
r = probe("wtrace_arm", lo="0x80090C80", hi="0x80090DE0")
print("  runtime:", r["runtime"])
print("  beetle :", r["beetle"])

print("\n== wtrace_ranges ==")
r = probe("wtrace_ranges")
print("  runtime shape:", shape(r["runtime"]))
print("  beetle  shape:", shape(r["beetle"]))
print("  runtime count:", r["runtime"].get("count"))
print("  beetle  count:", r["beetle"].get("count"))

print("\n== wtrace_stats ==")
r = probe("wtrace_stats")
print("  runtime:", r["runtime"])
print("  beetle :", r["beetle"])

print("\n== wtrace_dump count=3 ==")
r = probe("wtrace_dump", count=3)
for label in ("runtime", "beetle"):
    resp = r[label]
    print(f"  {label} top-level shape:", shape(resp))
    entries = resp.get("entries", [])
    print(f"  {label} entries: {len(entries)}")
    if entries:
        e0 = entries[0]
        print(f"  {label} entry[0] keys:", sorted(e0.keys()))
        print(f"  {label} entry[0] sample: addr={e0.get('addr')} new={e0.get('new')} pc={e0.get('pc')} w={e0.get('w')} frame={e0.get('frame')}")

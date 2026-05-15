"""Capture live state on whichever backend is reachable, RIGHT NOW.

Reads:
  - dispatch miss totals (runtime only)
  - frame counters
  - the two probe cells (0x80090CAC, 0x80090DA0) on both
  - surrounding range 0x80090C80..0x80090DE0 on both
  - hex diff of the surrounding range

No probes are armed — this is a pure ring/state read. Always-on rings
are preserved; we only read what's already captured.
"""
from __future__ import annotations
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _dbg import call, read_block, read_word, RUNTIME_PORT, BEETLE_PORT

PROBE_CELLS = [0x80090CAC, 0x80090DA0]
RANGE_LO    = 0x80090C80
RANGE_HI    = 0x80090DE0

def safe(port, cmd, **kw):
    try: return call(port, cmd, **kw)
    except Exception as e: return {"error": str(e)}

print("=== ping / frame / dispatch ===")
rp = safe(RUNTIME_PORT, "ping")
bp = safe(BEETLE_PORT,  "ping")
print(f"runtime: {rp}")
print(f"beetle : {bp}")

print("\n=== probe cells ===")
print(f"{'addr':<14} {'runtime':<12} {'beetle':<12} {'eq?':<4}")
for a in PROBE_CELLS:
    try: rv = read_word(RUNTIME_PORT, a)
    except Exception as e: rv = -1; rv_err = str(e)
    try: bv = read_word(BEETLE_PORT,  a)
    except Exception as e: bv = -1; bv_err = str(e)
    eq = "Y" if rv == bv else "N"
    print(f"0x{a:08X}   0x{rv:08X}   0x{bv:08X}   {eq}")

print(f"\n=== range 0x{RANGE_LO:08X}..0x{RANGE_HI:08X} (diff only) ===")
try:
    r = read_block(RUNTIME_PORT, RANGE_LO, RANGE_HI - RANGE_LO)
    b = read_block(BEETLE_PORT,  RANGE_LO, RANGE_HI - RANGE_LO)
    n_diff = 0
    print(f"{'addr':<12} runtime  beetle")
    for i in range(0, len(r), 4):
        rw = int.from_bytes(r[i:i+4], "little")
        bw = int.from_bytes(b[i:i+4], "little")
        if rw != bw:
            n_diff += 1
            print(f"0x{RANGE_LO+i:08X}  {rw:08X} {bw:08X}")
    print(f"\n{n_diff} divergent words out of {(RANGE_HI-RANGE_LO)//4}")
except Exception as e:
    print(f"range read failed: {e}")

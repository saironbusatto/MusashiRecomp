"""Scan generated/SCPH1001_full.c + dispatch table for codegen bugs.

Two passes:
1. Count how many sites had the predicate-init pattern (now fixed). This
   tells us the size of the bug class my fix addressed.
2. Find every psx_dispatch(cpu, 0xXXXX) target that ISN'T in the dispatch
   table — those are silent-miss candidates.
3. Find every `psx_dispatch(cpu, cpu->gpr[N])` indirect call — these go
   through the runtime dispatch table, so if a target isn't registered,
   the call silently fails.
4. Report remaining structural fragility points.

Run AFTER regen. Reads generated/ files only.
"""
import re, json, sys
from pathlib import Path
from collections import Counter, defaultdict

ROOT = Path(__file__).parent.parent
GEN = ROOT / 'generated'

full_c = (GEN / 'SCPH1001_full.c').read_text()
dispatch_c = (GEN / 'SCPH1001_dispatch.c').read_text()

# ---- 1. Confirm zero-init declarations (sanity check the fix) ----
buggy_decls = re.findall(r'int psx_taken_[0-9A-F]+ = \([^)]*\);', full_c)
zero_decls  = re.findall(r'int psx_taken_[0-9A-F]+ = 0;', full_c)
assignments = re.findall(r'^\s+psx_taken_[0-9A-F]+ = \(', full_c, re.M)
print(f"[1] predicate decl audit: buggy={len(buggy_decls)} zero_init={len(zero_decls)} assigns={len(assignments)}")
if buggy_decls:
    print("  ⚠ FIX REGRESSION — found buggy declarations:")
    for b in buggy_decls[:5]: print(f"    {b}")

def norm(addr):
    """Mirror SCPH1001_dispatch.c::normalize exactly.
    KSEG mask, then Kernel Part 2 remap (ROM 0x1FC10000+ -> RAM 0x500+),
    then Shell remap (RAM 0x30000+ -> ROM phys 0x1FC18000+)."""
    phys = addr & 0x1FFFFFFF
    if 0x1FC10000 <= phys <= 0x1FC17FFF:
        phys = phys - 0x1FC10000 + 0x500
    if 0x30000 <= phys <= 0x5AFFF:
        phys = phys - 0x30000 + 0x1FC18000
    return phys

# ---- 2. Find direct psx_dispatch targets and check against dispatch table ----
direct_targets = re.findall(r'psx_dispatch\(cpu,\s*0x([0-9A-Fa-f]+)u?\);', full_c)
direct_set = set(norm(int(t, 16)) for t in direct_targets)
print(f"\n[2] direct psx_dispatch targets (literal, normalized): {len(direct_set)} unique")

# Parse dispatch table — entries like { 0x00000500u, func_00000500 },
table_entries = re.findall(r'\{\s*0x([0-9A-Fa-f]+)u?\s*,\s*func_[0-9A-Fa-f]+', dispatch_c)
table_set = set(norm(int(e, 16)) for e in table_entries)
print(f"    dispatch table size: {len(table_set)} entries")

# Direct targets NOT in table → silent miss candidates (real bugs)
missing = sorted(direct_set - table_set)
print(f"    direct targets MISSING from table after normalization: {len(missing)}")
for m in missing[:25]:
    cnt = sum(1 for t in direct_targets if norm(int(t, 16)) == m)
    print(f"      0x{m:08X}  ({cnt} call site{'s' if cnt!=1 else ''})")

# ---- 3. Indirect dispatch sites (jalr/jr through gpr[N]) ----
indirect_sites = re.findall(r'psx_dispatch\(cpu,\s*cpu->gpr\[(\d+)\]\);', full_c)
print(f"\n[3] indirect psx_dispatch sites: {len(indirect_sites)} total")
gpr_use = Counter(indirect_sites)
print(f"    by register: {dict(gpr_use.most_common(8))}")

# ---- 4. Continuations: register_cross_function_target patterns ----
# Look for tail-call patterns: { cpu->pc = 0xXXXX; return; }
tail_calls = re.findall(r'cpu->pc = 0x([0-9A-Fa-f]+)u;\s*return;', full_c)
tail_set = set(norm(int(t, 16)) for t in tail_calls)
print(f"\n[4] tail-call (cpu->pc=...; return;) targets (normalized): {len(tail_set)} unique")
tail_missing = sorted(tail_set - table_set)
print(f"    tail-call targets MISSING from dispatch (real bugs): {len(tail_missing)}")
for m in tail_missing[:25]:
    cnt = sum(1 for t in tail_calls if norm(int(t, 16)) == m)
    print(f"      0x{m:08X}  ({cnt} site{'s' if cnt!=1 else ''})")

# ---- 5. Scan for chained-beq + delay-slot label-leader patterns ----
# Pattern: terminator at 0xA, delay slot at 0xA+4 is a label.
# This is the bug class we fixed; count how many sites benefited.
# Extract labels and beq sites.
labels = set(re.findall(r'^label_([0-9A-F]+):', full_c, re.M))
beq_sites = re.findall(r'/\* 0x([0-9A-F]+):\s+[0-9A-F]+\s+(beq|bne|bgez|bgtz|blez|bltz|bnel|beql)\b', full_c)
beq_addrs = [int(a, 16) for a, _ in beq_sites]
delay_is_label = sum(1 for a in beq_addrs if f"{a+4:08X}" in labels)
print(f"\n[5] chained-branch-with-delay-slot-label population:")
print(f"    total conditional branches: {len(beq_addrs)}")
print(f"    delay slot also a basic-block label: {delay_is_label}  (protected by zero-init fix)")

# ---- 6. Funnel: highest-frequency truly-missing targets ----
print(f"\n[6] highest-frequency dispatch-miss targets (real bugs):")
all_misses = Counter()
for t in direct_targets:
    addr = norm(int(t, 16))
    if addr not in table_set:
        all_misses[addr] += 1
for t in tail_calls:
    addr = norm(int(t, 16))
    if addr not in table_set:
        all_misses[addr] += 1
for addr, cnt in all_misses.most_common(30):
    print(f"      0x{addr:08X}  x{cnt}")

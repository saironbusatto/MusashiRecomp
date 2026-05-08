"""Extract dispatch-miss trampoline addresses from the audit and emit
seed entries to merge into recompiler/seeds/phase2_ghidra_seeds.json."""
import re, json
from pathlib import Path

ROOT = Path(__file__).parent.parent
GEN = ROOT / 'generated'

full_c = (GEN / 'SCPH1001_full.c').read_text()
dispatch_c = (GEN / 'SCPH1001_dispatch.c').read_text()

def norm(addr):
    phys = addr & 0x1FFFFFFF
    if 0x1FC10000 <= phys <= 0x1FC17FFF:
        phys = phys - 0x1FC10000 + 0x500
    if 0x30000 <= phys <= 0x5AFFF:
        phys = phys - 0x30000 + 0x1FC18000
    return phys

direct_targets = re.findall(r'psx_dispatch\(cpu,\s*0x([0-9A-Fa-f]+)u?\);', full_c)
tail_targets   = re.findall(r'cpu->pc = 0x([0-9A-Fa-f]+)u;\s*return;', full_c)
table_entries = re.findall(r'\{\s*0x([0-9A-Fa-f]+)u?\s*,\s*func_[0-9A-Fa-f]+', dispatch_c)
table_set = set(int(e, 16) for e in table_entries)

# Map each missed phys back to its callable virtual address (use 0xBFC... for ROM).
seeds = []
seen = set()
for t in list(direct_targets) + list(tail_targets):
    addr = int(t, 16)
    phys = norm(addr)
    if phys in table_set: continue
    if phys in seen:      continue
    seen.add(phys)
    # Only ROM-region misses get seeded (RAM is dirty_ram_dispatch territory).
    # 0x1FC10000-0x1FC42FFF maps to BIOS ROM in KSEG1 = 0xBFC10000-0xBFC42FFF.
    if 0x1FC00000 <= phys <= 0x1FC7FFFF:
        kseg1 = phys | 0xA0000000  # also valid; recompiler accepts either
        bfc   = phys | 0x80000000  # KSEG0 (cached) — convention used in seeds
        # phase2 seeds use 0xBFC... (KSEG1 uncached) per existing entries
        seed_addr = (phys & 0x1FFFFFFF) | 0xBFC00000 if phys >= 0x1FC00000 else (0x80000000 | phys)
        seeds.append({
            "address": f"0x{seed_addr:08X}",
            "label": f"trampoline_BFC{phys & 0x000FFFFF:05X}",
            "rationale": "audit-detected dispatch miss — A0/B0/C0 syscall trampoline"
        })

print(f"new seeds to add: {len(seeds)}")
seeds.sort(key=lambda s: s['address'])
for s in seeds[:10]:
    print(f"  {s}")

# Merge into existing seeds file.
seeds_path = ROOT / 'recompiler/seeds/phase2_ghidra_seeds.json'
data = json.loads(seeds_path.read_text())
existing_addrs = {s['address'].upper() for s in data['seeds']}
to_add = [s for s in seeds if s['address'].upper() not in existing_addrs]
print(f"\nactually new (not already in seeds): {len(to_add)}")
data['seeds'].extend(to_add)
data['seed_count'] = len(data['seeds'])
data['source'] += " + audit_2026_05_04 trampoline dispatch-miss"
seeds_path.write_text(json.dumps(data, indent=2))
print(f"updated {seeds_path}")

"""Output the complete list of dispatch-miss addresses, BIOS-virtual,
suitable for feeding into Ghidra create_function and into seeds."""
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

table_set = set(int(e, 16) for e in
                re.findall(r'\{\s*0x([0-9A-Fa-f]+)u?\s*,\s*func_[0-9A-Fa-f]+', dispatch_c))

def collect(pattern):
    out = set()
    for m in re.finditer(pattern, full_c):
        out.add(norm(int(m.group(1), 16)))
    return out

direct = collect(r'psx_dispatch\(cpu,\s*0x([0-9A-Fa-f]+)u?\);')
tails  = collect(r'cpu->pc = 0x([0-9A-Fa-f]+)u;\s*return;')
all_missing = sorted((direct | tails) - table_set)

# Convert phys to KSEG1 virt for ROM addresses, KSEG0 for RAM kernel
out = []
for phys in all_missing:
    if 0x1FC00000 <= phys <= 0x1FC7FFFF:
        virt = phys | 0xA0000000  # KSEG1 (uncached) — what BIOS uses
        # but seeds use 0xBFC...
        virt = (phys & 0x1FFFFFFF) | 0xBFC00000 if phys >= 0x1FC00000 else virt
    elif phys < 0x800000:
        virt = phys | 0x80000000  # kernel RAM
    else:
        virt = phys
    out.append((phys, virt))

print(f"total missing: {len(out)}")
print("BIOS-virt list (suitable for Ghidra create_function):")
for phys, virt in out:
    print(f"  0x{virt:08X}   (phys 0x{phys:08X})")

# also write json for later use
data = [{"addr": f"0x{v:08X}", "phys": f"0x{p:08X}"} for p, v in out]
(ROOT / 'tools' / '_missing_funcs.json').write_text(json.dumps(data, indent=2))
print(f"\nwrote tools/_missing_funcs.json")

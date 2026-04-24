#!/usr/bin/env python3
"""Audit GTE instructions: compare ROM contents vs generated code."""
import struct, re, sys

ROM_PATH = "bios/SCPH1001.BIN"
GEN_PATH = "generated/SCPH1001_full.c"
ROM_BASE = 0xBFC00000

# 1. Scan ROM for all COP2 (GTE) instructions
with open(ROM_PATH, "rb") as f:
    rom = f.read()

rom_gte = []  # (rom_addr, instruction_word)
for off in range(0, len(rom), 4):
    word = struct.unpack_from("<I", rom, off)[0]
    opcode = (word >> 26) & 0x3F
    if opcode == 0x12:  # COP2
        bit25 = (word >> 25) & 1
        if bit25 == 1:  # GTE command (not MFC2/MTC2/CFC2/CTC2)
            rom_addr = ROM_BASE + off
            rom_gte.append((rom_addr, word))

# Also find LWC2/SWC2 (COP2 load/store) — opcodes 0x32 (LWC2) and 0x3A (SWC2)
rom_lwc2_swc2 = []
for off in range(0, len(rom), 4):
    word = struct.unpack_from("<I", rom, off)[0]
    opcode = (word >> 26) & 0x3F
    if opcode in (0x32, 0x3A):  # LWC2 or SWC2
        rom_addr = ROM_BASE + off
        rom_lwc2_swc2.append((rom_addr, word))

# 2. Scan generated code for gte_execute addresses
gen_gte_addrs = set()
with open(GEN_PATH, "r") as f:
    for line in f:
        # Pattern: /* 0xBFCxxxxx: xxxxxxxx  gte cmd ... */
        m = re.search(r'/\* 0x(BFC[0-9A-Fa-f]{5}):', line)
        if m and 'gte' in line.lower():
            gen_gte_addrs.add(int(m.group(1), 16))
        # Also match gte_execute calls with preceding comment
        if 'gte_execute' in line:
            # Find the ROM address from the comment above
            pass

# Better approach: find all ROM addresses that have gte_execute
# by looking for the comment pattern before gte_execute
gen_gte_addrs2 = set()
with open(GEN_PATH, "r") as f:
    prev_addr = None
    for line in f:
        m = re.search(r'/\* 0x(BFC[0-9A-Fa-f]{5}):', line)
        if m:
            prev_addr = int(m.group(1), 16)
        if 'gte_execute' in line and prev_addr:
            gen_gte_addrs2.add(prev_addr)

# Also find all lwc2/swc2 in generated code
gen_cop2_load_addrs = set()
with open(GEN_PATH, "r") as f:
    for line in f:
        if 'lwc2' in line.lower() or 'swc2' in line.lower():
            m = re.search(r'/\* 0x(BFC[0-9A-Fa-f]{5}):', line)
            if m:
                gen_cop2_load_addrs.add(int(m.group(1), 16))

# 3. Compare
print(f"ROM GTE commands (COP2 bit25=1): {len(rom_gte)}")
print(f"ROM LWC2/SWC2 instructions: {len(rom_lwc2_swc2)}")
print(f"Generated gte_execute calls: {len(gen_gte_addrs2)}")
print(f"Generated lwc2/swc2 accesses: {len(gen_cop2_load_addrs)}")
print()

# Missing GTE commands
missing_gte = [(addr, word) for addr, word in rom_gte if addr not in gen_gte_addrs2]
print(f"MISSING GTE commands: {len(missing_gte)}")

# Group by 256-byte region to show function clusters
from collections import defaultdict
regions = defaultdict(list)
for addr, word in missing_gte:
    region = addr & ~0xFF
    func_code = (word & 0x3F)
    regions[region].append((addr, word, func_code))

print(f"Missing spread across {len(regions)} regions:")
for region in sorted(regions):
    entries = regions[region]
    codes = [f"0x{e[2]:02X}" for e in entries]
    addrs = [f"0x{e[0]:08X}" for e in entries]
    print(f"  0x{region:08X}: {len(entries)} missing — ops: {', '.join(codes)}")
    for a, w, c in entries:
        print(f"    0x{a:08X}: 0x{w:08X} (op {c:#04x})")

# Missing LWC2/SWC2
missing_loads = [(addr, word) for addr, word in rom_lwc2_swc2 if addr not in gen_cop2_load_addrs]
print(f"\nMissing LWC2/SWC2: {len(missing_loads)} out of {len(rom_lwc2_swc2)}")
if missing_loads:
    load_regions = defaultdict(list)
    for addr, word in missing_loads:
        load_regions[addr & ~0xFF].append(addr)
    for region in sorted(load_regions):
        print(f"  0x{region:08X}: {len(load_regions[region])} missing")

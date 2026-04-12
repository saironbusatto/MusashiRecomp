"""Synthesize a minimal demo of the branch emission contract by reading
the source bytes of a known hazardous branch from the BIOS, then printing
what the emitted C order will look like according to the new walker
contract. This is documentation/verification, not part of the build."""
import json
import struct
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with open(os.path.join(ROOT, "bios", "SCPH1001.BIN"), "rb") as f:
    rom = f.read()

# 0xbfc00270: bne $t2, $t3, target  ; raw 0x154bfff7
# 0xbfc00274: addiu $t2, $t2, 0x80  ; raw 0x214a0080  (delay slot writes $t2)
BRANCH_PC = 0xbfc00270
DELAY_PC  = 0xbfc00274

br_raw = struct.unpack_from("<I", rom, BRANCH_PC - 0xbfc00000)[0]
ds_raw = struct.unpack_from("<I", rom, DELAY_PC  - 0xbfc00000)[0]

print(f"branch  raw 0x{br_raw:08x} at 0x{BRANCH_PC:08x}")
print(f"delay   raw 0x{ds_raw:08x} at 0x{DELAY_PC:08x}")

# Decode BNE
op = (br_raw >> 26) & 0x3F  # 0x05 = BNE
rs = (br_raw >> 21) & 0x1F  # $t2 = 10
rt = (br_raw >> 16) & 0x1F  # $t3 = 11
simm = br_raw & 0xFFFF
if simm & 0x8000: simm -= 0x10000
target = BRANCH_PC + 4 + (simm * 4)

print(f"  op={op:#x}  rs={rs}  rt={rt}  simm={simm}  target=0x{target:08x}")
print()
print("Emitted C order under the corrected walker contract:")
print("---")
print(f"    /* 0x{BRANCH_PC:08x}: {br_raw:08x}  [pre-delay snapshot for 0x{BRANCH_PC:08x}] bne $t2, $t3, 0x{target:08x} */")
print(f"    uint32_t psx_brA_{BRANCH_PC:08X} = cpu->gpr[{rs}]; uint32_t psx_brB_{BRANCH_PC:08X} = cpu->gpr[{rt}];")
print(f"    /* 0x{DELAY_PC:08x}: {ds_raw:08x}  [delay slot of 0x{BRANCH_PC:08x}] addiu $t2, $t2, 128 */")
print(f"    cpu->gpr[10] = (uint32_t)((int32_t)cpu->gpr[10] + (128));")
print(f"    /* 0x{BRANCH_PC:08x}: {br_raw:08x}  bne $t2, $t3, 0x{target:08x} */")
print(f"    if (psx_brA_{BRANCH_PC:08X} != psx_brB_{BRANCH_PC:08X}) {{ cpu->pc = 0x{target:08X}u; return; }} cpu->pc = 0x{BRANCH_PC+8:08X}u; return;")
print("---")
print()
print("Notice:")
print("  1. The snapshot reads cpu->gpr[10] / cpu->gpr[11] BEFORE the delay slot.")
print("  2. The delay slot then mutates cpu->gpr[10] (adds 128).")
print("  3. The branch condition reads psx_brA_/psx_brB_ — the pre-delay values.")
print("  4. So the branch decision is correct even though the delay slot wrote to $t2.")

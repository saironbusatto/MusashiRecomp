#!/usr/bin/env python3
"""Decode a PSXRecomp savestate (.pst) header + CPU section, per boot_state.c.

The issue tracker reasoned about savestates by grepping the runtime's log
output. This reads the artifact directly instead, so a state's resume PC and
integrity fields are facts rather than inferences.

Layout (runtime/include/boot_state.h):
  BootStateHeader: 9 x uint32 (magic, version, bios_checksum, entry_pc,
                   codegen_hash, abi_tag, codegen_ver, section_count, reserved)
  then section_count records of: uint32 tag; uint32 pad; uint64 len; payload[len]
"""
import struct
import sys
from pathlib import Path

SECTIONS = {
    0x01: "CPU", 0x02: "RAM", 0x03: "SPAD", 0x04: "IRQ", 0x05: "TIMER",
    0x06: "CLOCK", 0x07: "GPU", 0x08: "VRAM", 0x09: "SPU", 0x0A: "SPURAM",
    0x0B: "CDROM", 0x0C: "DMA", 0x0D: "SIO", 0x0E: "DIRTY",
}

TEXT_END = 0x80074800  # SLUS_007.26 t_addr + t_size, from the EXE header


def decode(path):
    data = Path(path).read_bytes()
    hdr = struct.unpack_from("<9I", data, 0)
    magic, version, bios_ck, entry_pc, cgen_hash, abi_tag, cgen_ver, nsec, _res = hdr
    print(f"== {path}")
    print(f"   magic=0x{magic:08X} version={version} entry_pc=0x{entry_pc:08X}")
    print(f"   bios_checksum=0x{bios_ck:08X} codegen_hash=0x{cgen_hash:08X} "
          f"abi_tag=0x{abi_tag & 0xFFFFFFFF:08X} codegen_ver={cgen_ver}")
    print(f"   section_count={nsec}")

    off = struct.calcsize("<9I")
    found_cpu = False
    for _ in range(nsec):
        if off + 16 > len(data):
            print("   TRUNCATED section stream")
            break
        tag, _pad, length = struct.unpack_from("<IIQ", data, off)
        off += 16
        name = SECTIONS.get(tag, f"UNKNOWN(0x{tag:02X})")
        payload = data[off:off + length]
        if len(payload) != length:
            print(f"   section {name}: TRUNCATED "
                  f"({len(payload)} of {length} bytes)")
            break
        if tag == 0x01:
            found_cpu = True
            decode_cpu(payload)
        else:
            print(f"   section {name}: {length} bytes")
        off += length

    if not found_cpu:
        print("   NO CPU SECTION -- cannot determine resume PC")
    return off


def decode_cpu(p):
    # CpuRegs: gpr[32] then pc, hi, lo, cop0[32], gte_data[32], gte_ctrl[32]
    # Size check pins the layout instead of trusting the comment.
    off = 0
    gpr = struct.unpack_from("<32I", p, off); off += 128
    pc, hi, lo = struct.unpack_from("<3I", p, off); off += 12
    print(f"   CPU: pc=0x{pc:08X} hi=0x{hi:08X} lo=0x{lo:08X} "
          f"payload={len(p)}B")
    if pc >= TEXT_END:
        print(f"        -> ABOVE text end 0x{TEXT_END:08X}: overlay/RAM region")
    else:
        print(f"        -> within static text (< 0x{TEXT_END:08X})")
    print(f"        ra=0x{gpr[31]:08X} sp=0x{gpr[29]:08X} a0=0x{gpr[4]:08X}")
    cop0 = struct.unpack_from("<32I", p, off)
    print(f"        cop0[12]=SR=0x{cop0[12]:08X} cop0[13]=CAUSE=0x{cop0[13]:08X} "
          f"cop0[14]=EPC=0x{cop0[14]:08X}")
    exp = 128 + 12 + 32 * 4 + 32 * 4 + 32 * 4
    if len(p) != exp:
        print(f"        NOTE: expected {exp}B for gpr/pc/hi/lo/cop0/gte, "
              f"got {len(p)}B -- layout guess is wrong, treat pc with care")
    return pc


if __name__ == "__main__":
    for a in sys.argv[1:]:
        decode(a)
        print()
"""Read MIPS at the B0 call sites to identify what BIOS function each calls.
The t1 register holds the B0 function index when calling B0 trampoline."""
import sys, struct
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

call_sites = [0x800171F8, 0x800170BC, 0x8006825C]
# These are RA values (i.e., return-from-call PCs). The JAL instruction was at ra-8.

def disasm_li(insn):
    """Decode `li/addiu rt, rs, imm` or similar t1-loading instructions."""
    op = (insn >> 26) & 0x3F
    rs = (insn >> 21) & 0x1F
    rt = (insn >> 16) & 0x1F
    imm = insn & 0xFFFF
    if imm > 0x7FFF:
        imm = imm - 0x10000
    funct = insn & 0x3F
    if op == 0x9:  # addiu
        return f"addiu r{rt},r{rs},{imm:#x}"
    if op == 0xD:  # ori
        return f"ori r{rt},r{rs},{imm:#x}"
    if op == 0xF:  # lui
        return f"lui r{rt},{imm:#x}"
    if op == 0:    # SPECIAL
        if funct == 9:
            return f"jalr r{rs}"
        if funct == 8:
            return f"jr r{rs}"
        return f"SPECIAL funct={funct:#x}"
    if op == 0x3:  # jal
        target = (insn & 0x3FFFFFF) << 2
        return f"jal target_relative=0x{target:08x}"
    if op == 0x2:  # j
        target = (insn & 0x3FFFFFF) << 2
        return f"j target_relative=0x{target:08x}"
    return f"op={op:#x} insn={insn:08x}"

for site in call_sites:
    print(f"=== Call site (return-to PC) 0x{site:08X} ===")
    print(f"    JAL was at 0x{site-8:08X}")
    # Read 8 instructions before site (32 bytes back)
    base = site - 0x20
    r = _dbg.call(4470, "read_ram", addr=f"0x{base:08X}", len=0x30, timeout=3)
    if not r.get("ok"):
        print(f"    read_ram failed: {r}")
        continue
    b = bytes.fromhex(r["hex"])
    for i in range(0, len(b), 4):
        addr = base + i
        if addr >= site + 0x4: break
        insn = struct.unpack_from("<I", b, i)[0]
        mark = ""
        if addr == site-8: mark = " <-- JAL"
        if addr == site-4: mark = " <-- delay slot"
        if addr == site:   mark = " <-- return point"
        print(f"    {addr:08X}: {insn:08x}    {disasm_li(insn)}{mark}")
    print()

import sys, struct
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

addr = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x0005B40C
size = int(sys.argv[2], 0) if len(sys.argv) > 2 else 64
r = _dbg.call(4470, 'read_ram', addr=f'0x{addr:08X}', len=size, timeout=3)
b = bytes.fromhex(r['hex'])
for i in range(0, len(b), 4):
    insn = struct.unpack_from('<I', b, i)[0]
    op = (insn>>26)&0x3F
    rs = (insn>>21)&0x1F
    rt = (insn>>16)&0x1F
    rd = (insn>>11)&0x1F
    imm16 = insn&0xFFFF
    if imm16 > 0x7FFF: imm16 -= 0x10000
    funct = insn&0x3F
    target26 = insn & 0x3FFFFFF
    s = f'op={op:02x} rs={rs} rt={rt} rd={rd} imm={imm16:#x} funct={funct:02x}'
    # Try basic decoding
    if op == 0:
        if funct == 9: s = f'jalr r{rs}'
        elif funct == 8: s = f'jr r{rs}'
        elif funct == 0: s = 'nop'
        else: s = f'SPECIAL funct={funct:#x}'
    elif op == 9: s = f'addiu r{rt}, r{rs}, {imm16}'
    elif op == 0xD: s = f'ori r{rt}, r{rs}, 0x{insn&0xFFFF:x}'
    elif op == 0xF: s = f'lui r{rt}, 0x{insn&0xFFFF:x}'
    elif op == 0x2: s = f'j 0x{target26<<2:08x}'
    elif op == 0x3: s = f'jal 0x{target26<<2:08x}'
    elif op == 0x4: s = f'beq r{rs}, r{rt}, off={imm16:#x}'
    elif op == 0x5: s = f'bne r{rs}, r{rt}, off={imm16:#x}'
    elif op == 0x23: s = f'lw r{rt}, {imm16:#x}(r{rs})'
    elif op == 0x2B: s = f'sw r{rt}, {imm16:#x}(r{rs})'
    print(f'  0x{addr+i:08X}: {insn:08x}  {s}')

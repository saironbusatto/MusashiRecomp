"""Dump the 4 cursor handler stubs at 0x800321{5C,7C,9C,BC}."""
import socket, json, struct
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
def send(p):
    s.sendall((json.dumps(p)+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue

def disasm_word(w, pc):
    op = (w >> 26) & 0x3f
    rs = (w >> 21) & 0x1f
    rt = (w >> 16) & 0x1f
    rd = (w >> 11) & 0x1f
    sa = (w >> 6) & 0x1f
    fn = w & 0x3f
    imm = w & 0xffff
    simm = imm if imm < 0x8000 else imm - 0x10000
    target = (w & 0x3ffffff)
    jal_target = ((pc + 4) & 0xF0000000) | (target << 2)
    if op == 0:
        if fn == 0 and w == 0: return 'nop'
        if fn == 0x21:  return f'addu  ${rd}, ${rs}, ${rt}'
        if fn == 0x24:  return f'and   ${rd}, ${rs}, ${rt}'
        if fn == 0x25:  return f'or    ${rd}, ${rs}, ${rt}'
        if fn == 0x09:  return f'jalr  ${rd}, ${rs}'
        if fn == 0x08:  return f'jr    ${rs}'
    if op == 0x02: return f'j     0x{jal_target:08X}'
    if op == 0x03: return f'jal   0x{jal_target:08X}'
    if op == 0x04: return f'beq   ${rs}, ${rt}, +0x{(imm<<2)&0xffff:04X}'
    if op == 0x05: return f'bne   ${rs}, ${rt}, +0x{(imm<<2)&0xffff:04X}'
    if op == 0x09: return f'addiu ${rt}, ${rs}, {simm}'
    if op == 0x23: return f'lw    ${rt}, {simm}(${rs})'
    if op == 0x2b: return f'sw    ${rt}, {simm}(${rs})'
    if op == 0x0d: return f'ori   ${rt}, ${rs}, 0x{imm:X}'
    if op == 0x0f: return f'lui   ${rt}, 0x{imm:X}'
    return f'<unk op {op:02X}>'

print('=== full 0x80032xxx stubs (32 bytes each) ===')
for base, lbl in [(0x8003215C, 'cursor=4 stub @8003215C'),
                  (0x8003217C, 'cursor=5 stub @8003217C'),
                  (0x8003219C, 'cursor=6 stub @8003219C'),
                  (0x800321BC, 'extra stub  @800321BC'),
                  (0x800321DC, 'next        @800321DC')]:
    rr = send({'id':1,'cmd':'read_ram','addr':f'0x{base:08X}','len':0x40})
    b = bytes.fromhex(rr.get('hex',''))
    print(f'\n  -- {lbl} --')
    for i in range(0, len(b), 4):
        if i + 4 > len(b): break
        w = struct.unpack('<I', b[i:i+4])[0]
        print(f'    0x{base+i:08X}: 0x{w:08X}   {disasm_word(w, base+i)}')

s.close()

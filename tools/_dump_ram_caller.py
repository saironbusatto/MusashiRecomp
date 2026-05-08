"""Dump RAM bytes around 0x80037648 (the RA of the BFC1A420 caller)."""
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

def disasm(w, pc):
    op = (w >> 26) & 0x3f
    rs = (w >> 21) & 0x1f
    rt = (w >> 16) & 0x1f
    rd = (w >> 11) & 0x1f
    fn = w & 0x3f
    imm = w & 0xffff
    simm = imm if imm < 0x8000 else imm - 0x10000
    target = (w & 0x3ffffff)
    jal_target = ((pc + 4) & 0xF0000000) | (target << 2)
    if w == 0: return 'nop'
    if op == 0:
        if fn == 0x08: return f'jr    ${rs}'
        if fn == 0x09: return f'jalr  ${rd}, ${rs}'
        if fn == 0x21: return f'addu  ${rd}, ${rs}, ${rt}'
        if fn == 0x24: return f'and   ${rd}, ${rs}, ${rt}'
        if fn == 0x25: return f'or    ${rd}, ${rs}, ${rt}'
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

# Look at 0x80037600..0x80037800 to find the function calling BFC1A420
print("=== 0x80037600..0x80037800 (function around 0x80037648 RA) ===")
rr = send({'id':1,'cmd':'read_ram','addr':'0x80037600','len':0x200})
b = bytes.fromhex(rr.get('hex',''))
for i in range(0, len(b), 4):
    if i + 4 > len(b): break
    w = struct.unpack('<I', b[i:i+4])[0]
    addr = 0x80037600 + i
    marker = '  <-- RA after BFC1A420 jal' if addr == 0x80037648 else ''
    print(f'  0x{addr:08X}: 0x{w:08X}   {disasm(w, addr)}{marker}')

# Also check what's at the paired ROM address BFC17600..BFC17800 — should
# be different (we already saw BFC17640+ is NOPs but let's verify)
print("\n=== ROM 0xBFC17600..0xBFC17680 (kernel ROM source if it existed) ===")
rr = send({'id':1,'cmd':'read_ram','addr':'0xBFC17600','len':0x80})
b = bytes.fromhex(rr.get('hex',''))
for i in range(0, len(b), 4):
    if i + 4 > len(b): break
    w = struct.unpack('<I', b[i:i+4])[0]
    addr = 0xBFC17600 + i
    print(f'  0x{addr:08X}: 0x{w:08X}   {disasm(w, addr)}')
s.close()

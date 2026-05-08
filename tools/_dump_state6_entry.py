"""Dump state-6 entry (the active modal at YES/NO COPY confirm)."""
import socket, json, struct
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
def send(p):
    s.sendall((json.dumps(p) + '\n').encode())
    buf = b''
    while True:
        c = s.recv(1 << 20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue

ENTRY = 0x80110EC8 + 6 * 0x688
print(f"state-6 entry base = 0x{ENTRY:08X}  (idx=6, size=0x688)")
rr = send({'id':1,'cmd':'read_ram','addr':f'0x{ENTRY:08X}','len':0x80})
b = bytes.fromhex(rr.get('hex',''))
labels = {
    0x00: '+0x00',
    0x04: '+0x04 widget_id',
    0x08: '+0x08',
    0x0C: '+0x0C',
    0x10: '+0x10',
    0x14: '+0x14',
    0x18: '+0x18',
    0x1C: '+0x1C SQUARE',
    0x20: '+0x20 CIRCLE',
    0x24: '+0x24 TRIANGLE',
    0x28: '+0x28 CROSS',
    0x2C: '+0x2C UP',
    0x30: '+0x30 DOWN_or_LR',
    0x34: '+0x34 primary_handler',
    0x38: '+0x38 mask',
    0x3C: '+0x3C',
    0x40: '+0x40',
    0x44: '+0x44',
    0x48: '+0x48',
    0x4C: '+0x4C',
}
for i in range(0, len(b), 4):
    if i + 4 > len(b): break
    w = struct.unpack('<I', b[i:i+4])[0]
    lab = labels.get(i, f'+0x{i:02X}')
    print(f"  0x{ENTRY+i:08X} {lab:<28} = 0x{w:08X}")
s.close()

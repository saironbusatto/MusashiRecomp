"""Read the gate value mem[0x80079E2C] + structure pointers we need."""
import socket, json, struct
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
def send(p):
    s.sendall((json.dumps(p) + '\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue

for (a, ln, lbl) in [
    (0x80079E20, 0x20, 'around 0x80079E2C (gate)'),
    (0x80066BB8, 0x10, 'cursor BB8'),
    (0x80066B68, 0x50, 'button table B68'),
    (0x800EA868, 0x60, 'a0/a2 widget object 0xEA868'),
    (0x800EA784, 0x40, 'a1 reference 0xEA784'),
    (0x800EBC88, 0x60, 'alt widget object 0xEBC88'),
    (0x800EA8E8, 0x60, 'dispatcher a0 0xEA8E8'),
]:
    rr = send({'id':1,'cmd':'read_ram','addr':f'0x{a:08X}','len':ln})
    b = bytes.fromhex(rr.get('hex',''))
    print(f'=== {lbl} (0x{a:08X}) ===')
    for i in range(0, len(b), 4):
        if i + 4 > len(b): break
        w = struct.unpack('<I', b[i:i+4])[0]
        marker = ''
        if a + i == 0x80079E2C: marker = '  <-- GATE (==4 returns early)'
        print(f'  0x{a+i:08X}: 0x{w:08X}{marker}')
    print()
s.close()

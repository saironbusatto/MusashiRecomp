"""Dump every state-table entry (0..32) widget+button slots, find which one
has widget=0xA AND CROSS != 0 (the true modal commit entry)."""
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

ENTRY_BASE = 0x80110EC8
ENTRY_SIZE = 0x688

print(f"{'idx':>3} {'addr':>12} {'+04 widget':>12} "
      f"{'+1C SQ':>10} {'+20 CIR':>10} {'+24 TRI':>10} {'+28 X':>10} "
      f"{'+34 handler':>14} {'+38 mask':>10}")
for idx in range(32):
    addr = ENTRY_BASE + idx * ENTRY_SIZE
    rr = send({'id':1,'cmd':'read_ram','addr':f'0x{addr:08X}','len':0x40})
    b = bytes.fromhex(rr.get('hex',''))
    if len(b) < 0x40: continue
    widget   = struct.unpack('<I', b[0x04:0x08])[0]
    sq       = struct.unpack('<I', b[0x1C:0x20])[0]
    cir      = struct.unpack('<I', b[0x20:0x24])[0]
    tri      = struct.unpack('<I', b[0x24:0x28])[0]
    cross    = struct.unpack('<I', b[0x28:0x2C])[0]
    handler  = struct.unpack('<I', b[0x34:0x38])[0]
    mask     = struct.unpack('<I', b[0x38:0x3C])[0]
    flag = ' <-- widget=A' if widget == 0xA else ''
    if widget == 0xA and cross != 0: flag += ' *MODAL CANDIDATE*'
    print(f"{idx:>3} 0x{addr:08X} 0x{widget:>10X} "
          f"0x{sq:>8X} 0x{cir:>8X} 0x{tri:>8X} 0x{cross:>8X} "
          f"0x{handler:>12X} 0x{mask:>8X}{flag}")
s.close()

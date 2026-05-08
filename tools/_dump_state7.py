"""Dump full state-7 entry (0x688 bytes) and scan for handler-pointer slots."""
import socket, json, struct

s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
def send(p):
    s.sendall((json.dumps(p)+'\n').encode())
    buf=b''
    while True:
        c=s.recv(1<<20)
        if not c: break
        buf+=c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue

ENTRY = 0x80110EC8 + 7 * 0x688
r = send({'id':1,'cmd':'read_ram','addr':f'0x{ENTRY:08X}','len':0x688})
b = bytes.fromhex(r['hex'])

# Dump as 4-byte words; flag any value that looks like a handler PC
# (RAM 0x80030000..0x80042000, or ROM 0xBFC18000..0xBFC42800).
def looks_like_pc(v):
    return (0x80030000 <= v < 0x80043000) or (0xBFC18000 <= v < 0xBFC43000)

print(f"State-7 entry @ 0x{ENTRY:08X}, size 0x688 ({len(b)} bytes)\n")
print(f"  HEAD per handoff offsets:")
print(f"    +0x00            = 0x{struct.unpack('<I', b[0x00:0x04])[0]:08X}")
print(f"    +0x04 widget_id  = 0x{struct.unpack('<I', b[0x04:0x08])[0]:08X}")
print(f"    +0x1C SQUARE wid = 0x{struct.unpack('<I', b[0x1C:0x20])[0]:08X}")
print(f"    +0x20 CIRCLE wid = 0x{struct.unpack('<I', b[0x20:0x24])[0]:08X}")
print(f"    +0x24 TRIANGLE   = 0x{struct.unpack('<I', b[0x24:0x28])[0]:08X}")
print(f"    +0x28 CROSS wid  = 0x{struct.unpack('<I', b[0x28:0x2C])[0]:08X}")
print(f"    +0x34 handler PC = 0x{struct.unpack('<I', b[0x34:0x38])[0]:08X}")
print(f"    +0x38 hand. mask = 0x{struct.unpack('<I', b[0x38:0x3C])[0]:08X}")

print(f"\n  ALL handler-PC-looking entries (RAM 0x80030000-0x80043000 or ROM 0xBFC18000-0xBFC43000):")
for off in range(0, len(b)-3, 4):
    v = struct.unpack("<I", b[off:off+4])[0]
    if looks_like_pc(v):
        # Also show neighbors so we can guess the (handler, mask) pair.
        nxt = struct.unpack("<I", b[off+4:off+8])[0] if off+8 <= len(b) else 0
        print(f"    +0x{off:04X} = 0x{v:08X}    (next +0x{off+4:04X} = 0x{nxt:08X})")
s.close()

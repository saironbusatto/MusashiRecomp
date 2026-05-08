"""Scan state-entry table at 0x80110EC8 for entry+0x4 (widget_id).
There are 96 entries (0x60), each 0x688 bytes."""
import socket, json, struct
s = socket.create_connection(('127.0.0.1', 4370), timeout=15)
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

BASE = 0x80110EC8
STRIDE = 0x688
N = 0x60

print(f"{'idx':>4} {'addr':>10} {'+0x00':>6} {'+0x04':>6} {'+0x08':>6}")
seen_widgets = set()
hits_for_a = []
for i in range(N):
    a = BASE + i * STRIDE
    rr = send({'id':1,'cmd':'read_ram','addr':f'0x{a:08X}','len':16})
    b = bytes.fromhex(rr.get('hex',''))
    if len(b) < 16: continue
    f0, f4, f8, fc = struct.unpack('<IIII', b)
    if f4 != 0:  # only show populated
        print(f"{i:>4} 0x{a:08X} 0x{f0:04X} 0x{f4:04X} 0x{f8:04X}")
    seen_widgets.add(f4)
    if f4 == 0xA:
        hits_for_a.append((i, a, f0, f4, f8, fc))

print(f"\nUnique widget IDs seen: {sorted(seen_widgets)}")
print(f"\nEntries with +0x4 == 0xA: {len(hits_for_a)}")
for h in hits_for_a:
    print(f"  idx={h[0]} addr=0x{h[1]:08X}  +0x00={h[2]} +0x04={h[3]} +0x08={h[4]}")

s.close()

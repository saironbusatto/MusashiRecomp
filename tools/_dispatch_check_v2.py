"""dispatch_check tracks the EVER-DISPATCHED set since boot — no ring rolling.
Query candidate function entry PCs from Beetle's known press chain."""
import socket, json
def call(payload):
    s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
    s.sendall((json.dumps(dict(payload, id=1))+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<16)
        if not c: break
        buf += c
        if buf.endswith(b'\n'): break
    s.close()
    return json.loads(buf.decode())

CANDIDATES = [
    (0x8003A700, 'main_state=4 entry candidate'),
    (0x8003A710, 'main_state=4 writer PC (inside func)'),
    (0x80046470, 'SetStateForWidget alt entry'),
    (0x80046468, 'SetStateForWidget body'),
    (0x8004644C, 'SetStateForWidget candidate'),
    (0x80039840, 'coordinator dispatch table'),
    (0x800394B0, 'coordinator entry'),
    (0x800321BC, 'modal stub'),
    (0x800397B0, 'case-A coordinator'),
    (0x8003C640, 'cursor_mirror2 writer 1'),
    (0x80032300, 'cursor_mirror2 writer 2'),
    (0x80032164, 'caller of cursor_mirror2 writer 1 (Beetle)'),
    (0x80032174, 'caller of cursor_mirror2 writer 2 (Beetle)'),
    (0x800465E0, 'caller of SetStateForWidget body (Beetle)'),
    (0x80008A58, 'coord caller A'),
    (0x80030558, 'coord caller B'),
    (0x80031660, 'lookup caller'),
    (0x80030534, 'coord parent'),
    (0x80046664, 'lookup caller 2'),
]
print(f"=== Recomp dispatch_check — was each PC ever dispatched? ===")
print(f"{'PC':<14} {'found':<6} {'label'}")
for pc, lbl in CANDIDATES:
    r = call({'cmd':'dispatch_check','addr':f'0x{pc:08X}'})
    print(f"0x{pc:08X}  {str(r.get('found')):<6} {lbl}")

print(f"\nrecomp dispatch_seq total: {r.get('total','?')}")

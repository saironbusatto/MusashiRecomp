"""Sanity check: did recomp ever dispatch to the press-handler PCs we expect?
Query dirty_block_log for target ranges around Beetle's writer PCs.

Beetle writers seen on a working press:
  0x8003A710 (main_state=4 entry — the FIRST press response write)
  0x80046470/0x80046468 (SetStateForWidget)
  0x8003ACE4 (cursor_mirror2 writer)
  0x80032350 (cursor_mirror2 writer 2)
  0x80039864/0x8003987C (case-A coordinator close)

dirty_block_log records (target, ra, frame). target is the RAM block PC
that recomp dispatched to. So if recomp ever ran the press-handler chain,
we should see hits in these ranges.
"""
import socket, json
def call(payload):
    s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
    s.sendall((json.dumps(dict(payload, id=1))+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        if buf.endswith(b'\n'): break
    s.close()
    return json.loads(buf.decode())

WINDOWS = [
    (0x8003A700, 0x8003A720, 'main_state=4 entry / writer at 0x8003A710'),
    (0x80046460, 0x80046490, 'SetStateForWidget body (state_idx/widget writer)'),
    (0x80039840, 0x800398A0, 'coordinator + case-A path'),
    (0x8003C640, 0x8003C700, 'cursor_mirror2 writer 1'),
    (0x80032300, 0x80032380, 'cursor_mirror2 writer 2'),
    (0x8003ACE0, 0x8003AD00, 'cursor_mirror2 writer 3 (Beetle group B)'),
    (0x800394B0, 0x800394C0, 'coordinator entry'),
    (0x800321BC, 0x800321D0, 'modal stub'),
]

print("=== Recomp dirty_block_log: dispatches to press-handler windows ===\n")
for lo, hi, lbl in WINDOWS:
    r = call({'cmd':'dirty_block_log',
              'target_lo': f'0x{lo:08X}',
              'target_hi': f'0x{hi:08X}',
              'count': 8})
    emitted = r.get('emitted', 0)
    total = r.get('total', 0)
    avail = r.get('available', 0)
    entries = r.get('entries', [])
    print(f"[{lo:08X}..{hi:08X}] {lbl}")
    print(f"  total log entries scanned={avail} (of {total} ever),  emitted={emitted}")
    if entries:
        for e in entries[:3]:
            print(f"    seq={e.get('seq')}  target=0x{int(e['target'],16):08X}  ra=0x{int(e['ra'],16):08X}  frame={e.get('frame')}")
    print()

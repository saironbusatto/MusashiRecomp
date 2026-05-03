"""Dump Beetle SIO byte trace."""
import socket, json, sys

def call(d):
    s = socket.create_connection(('127.0.0.1', 4370)); s.settimeout(15.0)
    s.sendall((json.dumps(d) + '\n').encode())
    buf = b''
    while True:
        try: c = s.recv(65536)
        except socket.timeout: break
        if not c: break
        buf += c
        depth = 0; in_str = False; esc = False
        for b in buf:
            if esc: esc = False; continue
            if b == 0x5C: esc = True; continue
            if b == 0x22: in_str = not in_str; continue
            if in_str: continue
            if b == 0x7B: depth += 1
            elif b == 0x7D: depth -= 1
        if depth == 0 and buf.strip(): break
    s.close()
    return json.loads(buf.decode())

count = int(sys.argv[1]) if len(sys.argv) > 1 else 200
r = call({'id': 1, 'cmd': 'emu_sio_trace', 'count': str(count)})
print(f"total={r.get('total','?')} returned={r.get('count', '?')}")
for e in r.get('entries', []):
    tx = e.get('tx', '?')
    rx = e.get('rx', '?')
    ctrl = e.get('ctrl', '?')
    if isinstance(tx, int): tx = f'0x{tx:02X}'
    if isinstance(rx, int): rx = f'0x{rx:02X}'
    if isinstance(ctrl, int): ctrl = f'0x{ctrl:04X}'
    print(f"  seq={e.get('seq','?'):>6} tx={tx} rx={rx} ctrl={ctrl}")

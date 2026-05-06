"""Identical-wire-protocol smoke: same commands hit both processes.
psx-runtime on 4370, psx-beetle on 4380.
"""
import socket, json

def call(port, payload):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps(payload)+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<16)
        if not c: break
        buf += c
        if buf.endswith(b'\n'): break
    s.close()
    return json.loads(buf.decode())

PROBES = [
    ('pad_status',           {'cmd':'pad_status'}),
    ('read_ram BFC00000/16', {'cmd':'read_ram','addr':'0xBFC00000','len':16}),
    ('wtrace_ranges',        {'cmd':'wtrace_ranges'}),
    ('fntrace_arms',         {'cmd':'fntrace_arms'}),
]

for name, payload in PROBES:
    payload = dict(payload, id=1)
    r1 = call(4370, payload)
    r2 = call(4380, payload)
    print(f'{name}:')
    print(f'  recomp 4370: {r1}')
    print(f'  beetle 4380: {r2}')
    print()

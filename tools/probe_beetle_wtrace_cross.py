"""Smoke probe: reset Beetle wtrace, press CROSS, dump trace.

Run AFTER Beetle has booted to the shell (default 200+ frames).
Default-armed range is [0x7568..0x756C). Pressed pad 0xBFFF = CROSS only.
"""
import socket, json, sys


def call(d, timeout=15.0):
    s = socket.create_connection(('127.0.0.1', 4370))
    s.settimeout(timeout)
    s.sendall((json.dumps(d) + '\n').encode())
    buf = b''
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        depth = 0; instr = False; esc = False
        for b in buf:
            if esc: esc = False; continue
            if b == 0x5C: esc = True; continue
            if b == 0x22: instr = not instr; continue
            if instr: continue
            if b == 0x7B: depth += 1
            elif b == 0x7D: depth -= 1
        if depth == 0 and buf.strip():
            break
    s.close()
    return json.loads(buf.decode())


print('=== reset wtrace ===')
print(call({'id': 1, 'cmd': 'beetle_wtrace_reset'}))

print('=== press CROSS for 240 frames ===')
print(call({'id': 2, 'cmd': 'emu_press', 'buttons': 0xBFFF, 'frames': 240}))

print('=== settle 60 frames idle ===')
print(call({'id': 3, 'cmd': 'emu_step', 'count': 60}))

print()
r = call({'id': 4, 'cmd': 'beetle_wtrace', 'count': 1000})
total = r.get('total', 0)
count = r.get('count', 0)
print(f'total={total} returned={count}')

prev = {}
for e in r.get('entries', []):
    addr = e['addr']
    new = int(e['val'], 16) & 0xFF
    old_str = f'0x{prev[addr]:02X}' if addr in prev else ' ?? '
    prev[addr] = new
    state = 'OPEN' if (new & 1) == 0 else 'BUSY'
    print(f'#{e["seq"]:>4} f={e["frame"]:>5} {addr} {old_str}->0x{new:02X} ({state}) '
          f'pc={e["pc"]} ({e["region"]:>9}) ra={e["ra"]} slot={e["slot"]} sz={e["size"]}')

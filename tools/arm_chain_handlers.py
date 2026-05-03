"""Arm fn_trace for chain step handlers + key chain functions."""
import socket, json

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

# Chain step jump tables
arms = []
# jt1 — WRITE chain (10 entries)
arms += [0x00005244, 0x000052C4, 0x000052F8, 0x00005398, 0x000053D4,
         0x0000542C, 0x00005488, 0x000054CC, 0x000054FC, 0x00005538]
# jt2 — READ chain (13 entries)
arms += [0x000056E8, 0x00005768, 0x0000579C, 0x00005834, 0x00005870,
         0x000058B4, 0x000058E8, 0x00005918, 0x00005954, 0x00005990,
         0x00005A00, 0x00005A58, 0x00005AB0]
# jt3 — DELETE chain (4 entries)
arms += [0x00005BA4, 0x00005C24, 0x00005C58, 0x00005D48]
# Master chain step dispatchers + setup functions
arms += [0x00004D6C,  # master step
         0x00004F90,  # alt step (FUN_bfc14F90)
         0x00004D3C,  # ?
         0x000051F4,  # FUN_bfc14CF4 - dispatcher 1
         0x00005688,  # FUN_bfc15188 - dispatcher 2
         0x00005EF4,  # 0x02 OPEN writer
         0x00005FA8,  # 0x04 OPEN writer
         0x00004F54,  # BUSY writer
         0x000059A0,  # FUN_bfc159A0 - R-setup (per memo: 0x02 to gate)
         0x00005A04,  # FUN_bfc15A04 - W-setup (0x04)
         0x00005AB8,  # FUN_bfc15AB8 - D-setup (0x08)
         0x000043D0,  # B0:0x5B handler
         ]

for pc in arms:
    r = call({'id': 1, 'cmd': 'beetle_fntrace_arm', 'target': f'0x{pc:08X}'})
    if not r.get('ok'):
        print(f"FAIL arm 0x{pc:08X}: {r}")

print(f"armed {len(arms)} additional targets")
r = call({'id': 1, 'cmd': 'beetle_fntrace_arms'})
print(f"total armed now: {r['count']}")

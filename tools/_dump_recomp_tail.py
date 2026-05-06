"""Dump recomp wtrace tail post-press. Filter to modal cells.
Recomp wtrace cmd is `wtrace_dump`, NOT `wtrace`."""
import socket, json
def call(payload):
    s = socket.create_connection(('127.0.0.1', 4370), timeout=15)
    s.sendall((json.dumps(dict(payload, id=1))+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        if buf.endswith(b'\n'): break
    s.close()
    return json.loads(buf.decode())
def hx(v):
    if isinstance(v,str): return int(v,16) if v.startswith('0x') else int(v)
    return v

CELL = {
    0x80066BB8: 'cursor',
    0x80066BBC: 'cursor_mirror2',
    0x80066BC0: 'main_state',
    0x80078320: 'state_idx',
    0x80078324: 'widget',
    0x80078328: 'cursor_anim',
    0x80079F64: 'cursor_mirror',
}
NOISE_PC = {0x80046400, 0x80046404, 0x800463F0}

# ---- Probe what wtrace_dump returns ----
r = call({'cmd':'wtrace_dump','count':65000})
ents = r.get('entries', r.get('writes', []))
print(f"=== Recomp wtrace_dump: keys={list(r.keys())[:8]} entries={len(ents)} ===")
if ents:
    print(f"First entry shape: {list(ents[0].keys())}")

# Try to find the press response. Filter to non-noise.
def get_pc(e):
    for k in ('pc','PC','func','func_addr'):
        if k in e: return hx(e[k])
    return 0
def get_addr(e):
    for k in ('addr','address'):
        if k in e: return hx(e[k])
    return 0
def get_val(e):
    for k in ('val','value'):
        if k in e: return hx(e[k])
    return 0
def get_seq(e):
    return e.get('seq', '?')
def get_ra(e):
    for k in ('ra','RA','caller'):
        if k in e: return hx(e[k])
    return 0

filtered = [e for e in ents if get_pc(e) not in NOISE_PC]
print(f"Non-noise entries: {len(filtered)}")
print()
print("Last 30 non-noise entries (most recent at bottom):")
for e in filtered[-30:]:
    addr = get_addr(e); pc = get_pc(e); val = get_val(e); ra = get_ra(e); seq = get_seq(e)
    name = ''
    for ca, cn in CELL.items():
        if abs(addr - ca) <= 4:
            name = f' [{cn}'
            if addr != ca: name += f'+{addr-ca}'
            name += ']'
            break
    print(f"  seq={seq:>5} addr=0x{addr:08X}{name:<22} val=0x{val:08X} pc=0x{pc:08X} ra=0x{ra:08X}")

# Live cells
print("\n=== Live cells (recomp) ===")
for addr, name in CELL.items():
    rr = call({'cmd':'read_ram','addr':f'0x{addr:08X}','len':4})
    print(f"  {name:<16} 0x{addr:08X} = {rr.get('hex')}")

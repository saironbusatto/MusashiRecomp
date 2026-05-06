"""Dump Beetle wtrace + fntrace tail. Filter wtrace to non-cursor-anim writes."""
import socket, json
def call(payload):
    s = socket.create_connection(('127.0.0.1', 4380), timeout=10)
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
NOISE_PC = {0x80046400, 0x80046404, 0x800463F0}  # cursor anim

# ---- wtrace ----
r = call({'cmd':'wtrace','count':65000})
ents = r.get('entries', [])
print(f"=== Beetle wtrace: total={r.get('total')} returned={len(ents)} ===")
filtered = [e for e in ents if hx(e.get('pc',0)) not in NOISE_PC]
print(f"non-noise (excl PCs {[hex(p) for p in NOISE_PC]}): {len(filtered)}")
print()
print("Last 15 non-noise wtrace entries:")
for e in filtered[-15:]:
    addr = hx(e.get('addr',0))
    pc   = hx(e.get('pc',0))
    val  = hx(e.get('val',0))
    ra   = hx(e.get('ra',0))
    seq  = e.get('seq','?')
    name = ''
    for ca, cn in CELL.items():
        if abs(addr - ca) <= 4:
            name = f' [{cn}'
            if addr != ca: name += f'+{addr-ca}'
            name += ']'
            break
    print(f"  seq={seq:>5} addr=0x{addr:08X}{name:<22} val=0x{val:08X} pc=0x{pc:08X} ra=0x{ra:08X}")

# ---- fntrace ----
print("\n=== Beetle fntrace: ", end='')
r = call({'cmd':'fntrace_dump','count':65000})
ents = r.get('entries', [])
print(f"total={r.get('total')} returned={len(ents)} ===")
LBL = {
    0x8004644C:'SetStateForWidget?', 0x80046468:'SetStateForWidget body?',
    0x80046470:'SetStateForWidget alt?', 0x8003C640:'cm2 writer 1?',
    0x80032300:'cm2 writer 2?',        0x800394B0:'coordinator',
    0x800321BC:'modal stub',           0x80008A58:'coord caller A',
    0x80030558:'coord caller B',
}
groups = {}
for e in ents:
    t = hx(e.get('target',0))
    c = hx(e.get('caller',0))
    rax = hx(e.get('ra',0))
    a0 = hx(e.get('a0',0))
    key = (t, c, rax, a0)
    groups[key] = groups.get(key, 0) + 1
print()
for (t, c, ra, a0), n in sorted(groups.items(), key=lambda x: -x[1]):
    lbl = LBL.get(t, '?')
    print(f"  {n:>4}x  target=0x{t:08X} ({lbl})")
    print(f"           caller=0x{c:08X}  ra=0x{ra:08X}  a0=0x{a0:08X}")

# ---- Cells now ----
print("\n=== Live cells ===")
for addr, name in CELL.items():
    rr = call({'cmd':'read_ram','addr':f'0x{addr:08X}','len':4})
    print(f"  {name:<16} 0x{addr:08X} = {rr.get('hex')}")

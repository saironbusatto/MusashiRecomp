"""Build the divergence table per ChatGPT's deliverable spec:
event | Beetle PC/target/value/state/input | Recomp PC/target/value/state/input | first divergence

Sources:
  Beetle wtrace + fntrace (already collected on prior press, still in rings).
  Recomp dirty_block_log (now 4M entries) + wtrace (now 18 ranges) post-press.
"""
import socket, json
def call(port, payload):
    s = socket.create_connection(('127.0.0.1', port), timeout=15)
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

# --- Recomp wtrace (filtered, last entries) ---
print("==================================================================")
print("=== RECOMP wtrace — last 30 non-noise entries ===")
print("==================================================================")
r = call(4370, {'cmd':'wtrace_dump','count':100000})
ents = r.get('entries', [])
print(f"total entries={r.get('total','?')} returned={len(ents)}")
def get_pc(e):
    for k in ('pc','PC','func','func_addr'):
        if k in e: return hx(e[k])
    return 0
def get_addr(e):
    return hx(e.get('addr',0))
def get_val(e):
    for k in ('val','value','new'):
        if k in e: return hx(e[k])
    return 0
def get_seq(e): return e.get('seq', '?')
def get_ra(e):
    for k in ('ra','RA','caller'):
        if k in e: return hx(e[k])
    return 0

filt = [e for e in ents if get_pc(e) not in NOISE_PC]
print(f"non-noise: {len(filt)}\n")
for e in filt[-30:]:
    addr = get_addr(e); pc = get_pc(e); val = get_val(e); ra = get_ra(e); seq = get_seq(e)
    name = ''
    for ca, cn in CELL.items():
        if abs(addr - ca) <= 4:
            name = f' [{cn}'
            if addr != ca: name += f'+{addr-ca}'
            name += ']'
            break
    print(f"  seq={seq:>6} addr=0x{addr:08X}{name:<22} val=0x{val:08X} pc=0x{pc:08X} ra=0x{ra:08X}")

# --- Recomp dirty_block_log filtered to Beetle's press-handler windows ---
print("\n==================================================================")
print("=== RECOMP dirty_block_log — dispatches into press-handler windows ===")
print("==================================================================")
WINDOWS = [
    (0x8003A700, 0x8003A720, 'main_state=4 entry / writer PC 0x8003A710'),
    (0x80046460, 0x80046490, 'SetStateForWidget body'),
    (0x80039840, 0x800398A0, 'coordinator + case-A path'),
    (0x8003C640, 0x8003C700, 'cursor_mirror2 writer (group A/C)'),
    (0x80032300, 0x80032380, 'cursor_mirror2 writer (group C)'),
    (0x8003ACE0, 0x8003AD00, 'cursor_mirror2 writer (group B)'),
    (0x800394B0, 0x800394C0, 'coordinator entry'),
    (0x800321BC, 0x800321D0, 'modal stub'),
]
for lo, hi, lbl in WINDOWS:
    r = call(4370, {'cmd':'dirty_block_log',
              'target_lo': f'0x{lo:08X}',
              'target_hi': f'0x{hi:08X}',
              'count': 8})
    emitted = r.get('emitted', 0)
    avail = r.get('available', 0)
    total = r.get('total', 0)
    print(f"\n[{lo:08X}..{hi:08X}] {lbl}")
    print(f"  scanned={avail}  total={total}  matched={emitted}")
    for e in r.get('entries', [])[:5]:
        seq = e.get('seq')
        tgt = hx(e['target'])
        ra  = hx(e['ra'])
        a0  = hx(e.get('a0','0x0'))
        a1  = hx(e.get('a1','0x0'))
        fr  = e.get('frame','?')
        print(f"    seq={seq}  target=0x{tgt:08X}  ra=0x{ra:08X}  a0=0x{a0:08X}  a1=0x{a1:08X}  frame={fr}")

# --- Live cells, both backends ---
print("\n==================================================================")
print("=== LIVE CELLS (both backends, post-press) ===")
print("==================================================================")
print(f"{'cell':<16} {'addr':<10} {'beetle':<12} {'recomp':<12}")
for addr, name in CELL.items():
    rb = call(4380, {'cmd':'read_ram','addr':f'0x{addr:08X}','len':4})
    rr = call(4370, {'cmd':'read_ram','addr':f'0x{addr:08X}','len':4})
    print(f"{name:<16} 0x{addr:08X}  {rb.get('hex'):<12} {rr.get('hex'):<12}")

# --- Pad state, both ---
print("\n==================================================================")
print("=== PAD STATUS (both backends, now) ===")
print("==================================================================")
print(f"  recomp: {call(4370, {'cmd':'pad_status'})}")
print(f"  beetle: {call(4380, {'cmd':'pad_status'})}")

"""Press CROSS for 2 frames at state-6 YES/NO modal, capture rings.

Plan-step 2: trace what CROSS actually calls.
Arm fn_entry on:
  - primary_handler 0x800321BC (RAM dynamic stub)
  - +0x28 CROSS slot pointer (currently 0x0 — should never fire)
  - the substate dispatcher region 0xBFC1D000..0xBFC1F000
  - the action handler region 0xBFC18000..0xBFC1A000
  - any RAM dispatch into the dynamically-installed handlers
"""
import socket, json, time, struct

PORT = 4370
s = socket.create_connection(('127.0.0.1', PORT), timeout=20)
def send(p):
    s.sendall((json.dumps(p) + '\n').encode())
    buf = b''
    while True:
        c = s.recv(1 << 20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue
def to_int(v):
    if isinstance(v, str): return int(v, 16) if v.startswith('0x') else int(v)
    return v

def snap_state():
    out = {}
    for (a, ln) in [(0x80066940, 16), (0x80066BB8, 16), (0x80078320, 16),
                    (0x8007A180, 4), (0x80066B68, 0x50)]:
        rr = send({'id':1,'cmd':'read_ram','addr':f'0x{a:08X}','len':ln})
        b = bytes.fromhex(rr.get('hex',''))
        for i in range(0, len(b), 4):
            if i+4 > len(b): break
            w = struct.unpack('<I', b[i:i+4])[0]
            out[f"0x{a+i:08X}"] = w
    fc = send({'id':1,'cmd':'freeze_check','window':32})
    out['_current_func'] = fc.get('current_func')
    out['_last_store'] = fc.get('last_store_pc')
    return out

print("=== BEFORE press ===")
before = snap_state()
print(f"  state_idx     = 0x{before['0x80078320']:08X}")
print(f"  widget_id     = 0x{before['0x80078324']:08X}")
print(f"  cursor (BB8)  = 0x{before['0x80066BB8']:08X}")
print(f"  substate (40) = 0x{before['0x80066940']:08X}")
print(f"  +6948 main    = 0x{before['0x80066948']:08X}")
print(f"  modal gate    = 0x{before['0x8007A180']:08X}")
print(f"  cur_fn        = {before['_current_func']}")
print(f"  last_store    = {before['_last_store']}")

# Clear rings
send({'id':1,'cmd':'fn_clear'})
send({'id':1,'cmd':'wtrace_clear'})

press_word = 0xFFFF & ~0x4000  # CROSS
print(f"\n=== pressing CROSS (mask 0x{press_word:04X}) for 2 frames ===")
print(send({'id':1,'cmd':'press','buttons':press_word,'frames':2}))
time.sleep(0.15)

print("\n=== AFTER press ===")
after = snap_state()
for k in sorted(set(before) | set(after)):
    if k.startswith('_'): continue
    bv, av = before.get(k), after.get(k)
    diff = ' <CHANGED>' if bv != av else ''
    print(f"  {k} = 0x{av:08X}{diff}")
print(f"  cur_fn={after['_current_func']}  last_store={after['_last_store']}")

# fn_entry totals
fs = send({'id':1,'cmd':'fn_stats'})
total = fs.get('entry_total', 0)
print(f"\n=== fn_entry total since clear: {total} ===")

# Look at wide ROM ranges where dispatch lives
ranges = [
    ('0x1FC18000', '0x1FC1A000', 'shell action region'),
    ('0x1FC1D000', '0x1FC1F000', 'substate dispatcher'),
    ('0x1FC19000', '0x1FC1A000', 'modal commit region'),
    ('0x1FC25000', '0x1FC26000', 'input/poller region'),
    ('0x1FC32000', '0x1FC33000', 'kernel 0x800321BC area (?)'),
    ('0x80032000', '0x80033000', 'RAM dynamic handlers'),
]
for lo, hi, label in ranges:
    r = send({'id':1,'cmd':'fn_entry_dump',
              'addr_lo':lo,'addr_hi':hi,'seq_lo':'0','count':80})
    es = r.get('entries', [])
    if es:
        print(f"\n  -- {label} ({lo}..{hi}): {len(es)} hits --")
        for e in es[:20]:
            print(f"    seq={e['seq']:>4} fr={e['frame']} func={e['func']} "
                  f"ra={e['ra']} a0={e['a0']} a1={e.get('a1')} a2={e.get('a2')}")

# Total wtrace
ws = send({'id':1,'cmd':'wtrace_stats'})
wtot = ws.get('total', 0)
print(f"\n=== wtrace total since clear: {wtot} ===")
# All writes to button table + state block + cursor
r = send({'id':1,'cmd':'wtrace_dump','start':0,'count':min(wtot, 800)})
groups = {'button_tbl': [], 'state_blk': [], 'cursor': [], 'state_idx': []}
for e in r.get('entries', []):
    a = to_int(e['addr'])
    if 0x66B68 <= a < 0x66BB8: groups['button_tbl'].append(e)
    elif 0x66940 <= a < 0x66960: groups['state_blk'].append(e)
    elif 0x66BB8 <= a < 0x66BD0: groups['cursor'].append(e)
    elif 0x78320 <= a < 0x78340: groups['state_idx'].append(e)
for name, es in groups.items():
    if es:
        print(f"\n  -- wtrace {name}: {len(es)} writes --")
        for e in es[-15:]:
            print(f"    fr={e.get('frame','?')} addr=0x{to_int(e['addr']):08X} "
                  f"old=0x{to_int(e.get('old',0)):08X} -> new=0x{to_int(e.get('new',0)):08X} "
                  f"by func=0x{to_int(e.get('func',0)):08X} ra=0x{to_int(e.get('ra',0)):08X}")

s.close()

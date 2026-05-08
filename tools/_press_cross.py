"""Press CROSS programmatically and capture state + fn_entry diff."""
import socket, json, time, struct
PORT = 4370

s = socket.create_connection(('127.0.0.1', PORT), timeout=15)
def send(p):
    s.sendall((json.dumps(p) + '\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue

def to_int(v):
    if isinstance(v,str): return int(v,16) if v.startswith('0x') else int(v)
    return v

def snapshot():
    out = {}
    for (a, ln) in [(0x80066940, 16), (0x80066BB8, 16), (0x80078320, 16),
                    (0x8007A180, 4)]:
        rr = send({'id':1,'cmd':'read_ram','addr':f'0x{a:08X}','len':ln})
        b = bytes.fromhex(rr.get('hex',''))
        for i in range(0, len(b), 4):
            if i+4 > len(b): break
            w = struct.unpack('<I', b[i:i+4])[0]
            out[f"0x{a+i:08X}"] = w
    fc = send({'id':1,'cmd':'freeze_check','window':32})
    out['_current_func'] = fc.get('current_func')
    return out

# 1. Snapshot before press
print("=== BEFORE press ===")
before = snapshot()
for k, v in before.items():
    print(f"  {k} = {v if isinstance(v,str) else f'0x{v:08X}'}")

# 2. Clear fn_entry/wtrace
send({'id':1,'cmd':'fn_clear'})
send({'id':1,'cmd':'wtrace_clear'})

# 3. Press CROSS for 4 frames (mask: 0xFFFF = idle, ~0x4000 = press CROSS)
press_word = 0xFFFF & ~0x4000   # 0xBFFF — CROSS pressed
print(f"\n=== pressing CROSS (0x{press_word:04X}) for 4 frames ===")
print(send({'id':1,'cmd':'press','buttons':press_word,'frames':4}))

# 4. Wait for it to land
time.sleep(0.2)

# 5. Snapshot after press
print("\n=== AFTER press ===")
after = snapshot()
for k, v in after.items():
    bv = before.get(k)
    diff = ' (CHANGED)' if bv != v else ''
    val = v if isinstance(v,str) else f'0x{v:08X}'
    print(f"  {k} = {val}{diff}")

# 6. fn_entry tail
stats = send({'id':1,'cmd':'fn_stats'})
total = stats.get('entry_total',0)
print(f"\n=== fn_entry total since clear: {total} ===")
seq_lo = max(0, total - 60)
r = send({'id':1,'cmd':'fn_entry_dump',
          'addr_lo':'0x1FC00000','addr_hi':'0x1FC50000',
          'seq_lo':str(seq_lo),'count':60})
for e in r.get('entries', []):
    print(f"  seq={e['seq']} fr={e['frame']} func={e['func']} ra={e['ra']} "
          f"a0={e['a0']} a1={e.get('a1')}")

# 7. wtrace tail
ws = send({'id':1,'cmd':'wtrace_stats'})
print(f"\n=== wtrace total since clear: {ws.get('total',0)} ===")
wt = send({'id':1,'cmd':'wtrace_dump','start':0,'count':80})
for e in wt.get('entries', []):
    addr = to_int(e['addr'])
    print(f"  fr={e.get('frame','?')} addr=0x{addr:08X} "
          f"old=0x{to_int(e.get('old',0)):08X} new=0x{to_int(e.get('new',0)):08X} "
          f"func=0x{to_int(e.get('func',0)):08X} ra=0x{to_int(e.get('ra',0)):08X}")

s.close()

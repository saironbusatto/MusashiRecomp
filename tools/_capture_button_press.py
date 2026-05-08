"""After user presses CROSS once, capture wtrace + fn_entry to see who
wrote to the button-state table and what."""
import socket, json, struct
s = socket.create_connection(('127.0.0.1', 4370), timeout=20)
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

# 1. Snapshot button table
print("=== button-table snapshot AFTER press ===")
rr = send({'id':1,'cmd':'read_ram','addr':'0x80066B68','len':0x50})
b = bytes.fromhex(rr.get('hex',''))
for i in range(0, len(b), 4):
    if i+4 > len(b): break
    w = struct.unpack('<I', b[i:i+4])[0]
    if w != 0:
        sw = struct.unpack('<i', b[i:i+4])[0]
        print(f"  0x{0x80066B68+i:08X} = 0x{w:08X}  ({sw})")

# 2. wtrace dump filtered to button-table writes
print("\n=== writes to 0x66B68..0x66BB8 since clear ===")
ws = send({'id':1,'cmd':'wtrace_stats'})
print(f"  total wtrace: {ws.get('total',0)}")
total = ws.get('total',0)
r = send({'id':1,'cmd':'wtrace_dump','start':0,'count':min(total,500)})
n = 0
for e in r.get('entries', []):
    addr = to_int(e['addr'])
    if 0x66B68 <= addr < 0x66BB8:
        print(f"  fr={e.get('frame','?')} addr=0x{addr:08X} "
              f"old=0x{to_int(e.get('old',0)):08X} new=0x{to_int(e.get('new',0)):08X} "
              f"func=0x{to_int(e.get('func',0)):08X} ra=0x{to_int(e.get('ra',0)):08X}")
        n += 1
print(f"  ({n} button-table writes)")

# 3. fn_entry hits to FUN_bfc19a58 + FUN_bfc24f00 + FUN_bfc228d8 + func_0xb003d200
print("\n=== specific fn_entry hits since clear ===")
fs = send({'id':1,'cmd':'fn_stats'})
total = fs.get('entry_total',0)
print(f"  total fn_entry: {total}")
for (lo, hi, name) in [
    (0x1FC19A58, 0x1FC19A5C, 'FUN_bfc19a58 (commit)'),
    (0x1FC24F00, 0x1FC24F04, 'FUN_bfc24f00 (poller)'),
    (0x1FC228D8, 0x1FC228DC, 'FUN_bfc228d8 (bc0=7 setter)'),
    (0x1FC25200, 0x1FC25204, 'func_0xb003d200 (input idx)'),
    (0x1FC251A8, 0x1FC251AC, 'func_0xb003d1a8 (input read)'),
]:
    r = send({'id':1,'cmd':'fn_entry_dump',
              'addr_lo':f'0x{lo:08X}','addr_hi':f'0x{hi:08X}',
              'seq_lo':'0','count':30})
    es = r.get('entries', [])
    print(f"  {name}: {len(es)} hits")
    for e in es[:3]:
        print(f"    seq={e['seq']} fr={e['frame']} a0={e['a0']} a1={e.get('a1')}")

# 4. Show last 30 wtrace anywhere in watched ranges
print("\n=== ALL last 30 wtrace entries ===")
r = send({'id':1,'cmd':'wtrace_dump','start':max(0,total-30),'count':30})
for e in r.get('entries', []):
    addr = to_int(e['addr'])
    print(f"  fr={e.get('frame','?')} addr=0x{addr:08X} "
          f"old=0x{to_int(e.get('old',0)):08X} new=0x{to_int(e.get('new',0)):08X} "
          f"ra=0x{to_int(e.get('ra',0)):08X}")

s.close()

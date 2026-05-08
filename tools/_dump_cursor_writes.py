"""Dump writes hitting the cursor block during the wedge."""
import socket, json
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
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

stats = send({'id':1,'cmd':'wtrace_stats'})
print('stats:', stats)
total = stats.get('total', 0)
start = max(0, total - 200)
r = send({'id':1,'cmd':'wtrace_dump','start':start,'count':200})
print(f"\nwrites in cursor block (last 200):")
n = 0
for e in r.get('entries', []):
    addr = to_int(e['addr'])
    if 0x66BB8 <= addr <= 0x66BCC:
        print(f"  fr={e.get('frame','?')} addr=0x{addr:08X} "
              f"old=0x{to_int(e.get('old',0)):08X} new=0x{to_int(e.get('new',0)):08X} "
              f"func=0x{to_int(e.get('func',0)):08X} ra=0x{to_int(e.get('ra',0)):08X}")
        n += 1
print(f"\ntotal cursor-block writes: {n}")

# Also show ALL last 30 writes (any addr in watched ranges)
print("\nALL last 30 writes (any watched range):")
start2 = max(0, total - 30)
r2 = send({'id':1,'cmd':'wtrace_dump','start':start2,'count':30})
for e in r2.get('entries', []):
    addr = to_int(e['addr'])
    print(f"  fr={e.get('frame','?')} addr=0x{addr:08X} "
          f"old=0x{to_int(e.get('old',0)):08X} new=0x{to_int(e.get('new',0)):08X} "
          f"func=0x{to_int(e.get('func',0)):08X} ra=0x{to_int(e.get('ra',0)):08X}")

s.close()

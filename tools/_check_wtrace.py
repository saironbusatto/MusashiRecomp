"""Verify wtrace setup + capture a clean press window."""
import socket, json, time, struct
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
    if isinstance(v, str): return int(v, 16) if v.startswith('0x') else int(v)
    return v

# Show currently configured ranges
print("=== current wtrace ranges ===")
print(json.dumps(send({'id':1,'cmd':'wtrace_ranges'}), indent=2))

# Set up known-good wide range covering shell state
send({'id':1,'cmd':'wtrace_range','lo':'0x80066000','hi':'0x80080000'})
print("\n=== ranges after wtrace_range ===")
print(json.dumps(send({'id':1,'cmd':'wtrace_ranges'}), indent=2))

# Clear and arm
send({'id':1,'cmd':'wtrace_clear'})
send({'id':1,'cmd':'fn_clear'})

# Press
press_word = 0xFFFF & ~0x4000
print(f"\n=== pressing CROSS for 4 frames ===")
print(send({'id':1,'cmd':'press','buttons':press_word,'frames':4}))
time.sleep(0.4)

# Stats
ws = send({'id':1,'cmd':'wtrace_stats'})
print(f"\n=== wtrace stats ===")
print(json.dumps(ws, indent=2))

# Dump
r = send({'id':1,'cmd':'wtrace_dump','start':0,'count':500})
ents = r.get('entries', [])
print(f"\n=== {len(ents)} wtrace entries; grouping by addr ===")
by_addr = {}
for e in ents:
    a = to_int(e['addr'])
    if a not in by_addr: by_addr[a] = []
    by_addr[a].append(e)
for a in sorted(by_addr):
    es = by_addr[a]
    last = es[-1]
    print(f"  0x{a:08X}: {len(es)} writes, last new=0x{to_int(last.get('new',0)):08X} "
          f"by func=0x{to_int(last.get('func',0)):08X} ra=0x{to_int(last.get('ra',0)):08X} "
          f"frame={last.get('frame','?')}")

# Specifically dump writes to 0x80078328
print("\n=== writes to 0x80078328 ===")
for e in ents:
    if to_int(e['addr']) == 0x00078328:
        print(f"  fr={e.get('frame','?')} old=0x{to_int(e.get('old',0)):08X} -> "
              f"new=0x{to_int(e.get('new',0)):08X} by func=0x{to_int(e.get('func',0)):08X} "
              f"ra=0x{to_int(e.get('ra',0)):08X} pc=0x{to_int(e.get('pc',0)):08X}")

s.close()

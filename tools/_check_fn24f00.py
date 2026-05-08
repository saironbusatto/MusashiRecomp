"""Has FUN_bfc24f00 been called during this runtime session?"""
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

stats = send({'id':1,'cmd':'fn_stats'})
total = stats.get('entry_total', 0)
print(f"fn_entry total = {total}")

# Search for hits to 0x1FC24F00 across whole ring
hits = 0
chunk = 2000
checked = 0
while checked < min(total, 200000):
    seq_lo = max(0, total - checked - chunk)
    r = send({'id':1,'cmd':'fn_entry_dump',
              'addr_lo':'0x1FC24F00','addr_hi':'0x1FC24F04',
              'seq_lo':str(seq_lo),'count':chunk})
    es = r.get('entries', [])
    hits += len(es)
    if es:
        print(f"  found {len(es)} hits in seq range [{seq_lo}, {seq_lo+chunk})")
        for e in es[:3]:
            print(f"    seq={e['seq']} fr={e['frame']} func={e['func']} ra={e['ra']} a0={e['a0']}")
    checked += chunk
    if seq_lo == 0: break

print(f"\nTotal FUN_bfc24f00 entries across {min(total,200000)} fn_entry seq: {hits}")

# Also check FUN_bfc19a58
print("\n--- FUN_bfc19a58 (0x1FC19A58) ---")
r = send({'id':1,'cmd':'fn_entry_dump',
          'addr_lo':'0x1FC19A58','addr_hi':'0x1FC19A5C',
          'seq_lo':'0','count':2000})
es = r.get('entries', [])
print(f"hits in first 2000 seq: {len(es)}")
for e in es[:5]:
    print(f"    seq={e['seq']} fr={e['frame']} func={e['func']} ra={e['ra']} a0={e['a0']}")

s.close()

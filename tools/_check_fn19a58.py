"""Has FUN_bfc19a58 (COPY commit handler) been called?"""
import socket, json
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
def send(p):
    s.sendall((json.dumps(p)+'\n').encode())
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
hits = 0
chunk = 4000
for off in range(0, min(total, 800000), chunk):
    seq_lo = max(0, total - off - chunk)
    r = send({'id':1,'cmd':'fn_entry_dump',
              'addr_lo':'0x1FC19A58','addr_hi':'0x1FC19A5C',
              'seq_lo':str(seq_lo),'count':chunk})
    es = r.get('entries', [])
    if es:
        for e in es:
            print(f"seq={e['seq']} fr={e['frame']} func={e['func']} ra={e['ra']} a0={e['a0']}")
            hits += 1
        if hits > 10: break
    if seq_lo == 0: break
print(f"\nFUN_bfc19a58 calls found in ring: {hits}")
s.close()

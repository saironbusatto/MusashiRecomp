"""Count fn_entry hits in a specific seq window."""
import socket, json, sys
seq_lo = sys.argv[1]
batch = int(sys.argv[2]) if len(sys.argv) > 2 else 10000
s = socket.create_connection(('127.0.0.1', 4370), timeout=600)
s.sendall((json.dumps({
    'id': 1, 'cmd': 'fn_entry_dump',
    'count': batch,
    'seq_lo': seq_lo, 'seq_hi': '999999999999',
    'reverse': False,
}) + '\n').encode())
buf = b''
while True:
    c = s.recv(65536)
    if not c: break
    buf += c
    if buf.strip().endswith(b'}'):
        try: json.loads(buf.decode()); break
        except: continue
r = json.loads(buf.decode())
print(f"total={r.get('total')} oldest={r.get('oldest')} returned={len(r.get('entries', []))}")
ents = r.get('entries', [])
if ents:
    print(f"first seq={ents[0]['seq']} frame={ents[0]['frame']}")
    print(f"last  seq={ents[-1]['seq']} frame={ents[-1]['frame']}")
hist = {}
for e in ents:
    k = e['func']
    hist[k] = hist.get(k, 0) + 1
for k in sorted(hist, key=lambda x: -hist[x])[:30]:
    print(f"  {k}: {hist[k]}")

"""Count fn_entry hits per func in the ring."""
import socket, json, sys
batch = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
s = socket.create_connection(('127.0.0.1', 4370), timeout=600)
s.sendall((json.dumps({
    'id': 1, 'cmd': 'fn_entry_dump',
    'count': batch,
    'seq_lo': '0', 'seq_hi': '999999999999',
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
hist = {}
for e in r.get('entries', []):
    k = e['func']
    hist[k] = hist.get(k, 0) + 1
for k in sorted(hist, key=lambda x: -hist[x]):
    print(f"  {k}: {hist[k]}")

"""Show top hottest functions in last N entries (uses default window)."""
import socket, json, sys
batch = int(sys.argv[1]) if len(sys.argv) > 1 else 50000
s = socket.create_connection(('127.0.0.1', 4370), timeout=600)
s.sendall((json.dumps({
    'id': 1, 'cmd': 'fn_entry_dump', 'count': batch, 'reverse': True,
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
ents = r.get('entries', [])
print(f"window: {ents[-1]['frame'] if ents else '?'} .. {ents[0]['frame'] if ents else '?'} ({len(ents)} entries)")
hist = {}
for e in ents:
    hist[e['func']] = hist.get(e['func'], 0) + 1
for k in sorted(hist, key=lambda x: -hist[x])[:20]:
    print(f"  {k}: {hist[k]}")

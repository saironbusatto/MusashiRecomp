"""Dump wtrace entries for a given physical addr range."""
import socket, json, sys
lo = sys.argv[1]
hi = sys.argv[2]
count = int(sys.argv[3]) if len(sys.argv) > 3 else 100
s = socket.create_connection(('127.0.0.1', 4370), timeout=120)
s.sendall((json.dumps({
    'id': 1, 'cmd': 'wtrace_dump',
    'addr_lo': lo, 'addr_hi': hi, 'count': count,
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
for e in r.get('entries', [])[:count]:
    print(f"  seq={e.get('seq')} addr={e.get('addr')} val={e.get('val')} sz={e.get('size')} pc={e.get('pc')} frame={e.get('frame')}")

"""Dump 50 most-recent fn_entry entries."""
import socket, json, sys
s = socket.create_connection(('127.0.0.1', 4370), timeout=120)
s.sendall((json.dumps({
    'id': 1, 'cmd': 'fn_entry_dump',
    'count': 50, 'reverse': True,
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
for e in r.get('entries', [])[:50]:
    print(f"  seq={e['seq']} func={e['func']} ra={e['ra']} a0={e['a0']} frame={e['frame']}")

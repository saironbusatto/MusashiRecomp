"""Force full ring scan with seq_lo=0. Args: addr_lo addr_hi"""
import socket, json, sys
addr_lo = sys.argv[1]
addr_hi = sys.argv[2]
s = socket.create_connection(('127.0.0.1', 4370), timeout=600)
s.sendall((json.dumps({
    'id': 1, 'cmd': 'fn_entry_dump',
    'addr_lo': addr_lo, 'addr_hi': addr_hi,
    'seq_lo': '0', 'seq_hi': '999999999999', 'count': 100,
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
print(f"total={r.get('total')} oldest={r.get('oldest')} seq_lo={r.get('seq_lo')} seq_hi={r.get('seq_hi')} returned={len(r.get('entries', []))}")
for e in r.get('entries', [])[:20]:
    print(f"  seq={e['seq']} func={e['func']} ra={e['ra']} a0={e['a0']} frame={e['frame']}")

"""fn_entry_dump filter showing $v0 (return value visible at entry to caller)."""
import socket, json, sys
addr_lo = sys.argv[1]
addr_hi = sys.argv[2]
s = socket.create_connection(('127.0.0.1', 4370), timeout=120)
s.sendall((json.dumps({
    'id': 1, 'cmd': 'fn_entry_dump',
    'addr_lo': addr_lo, 'addr_hi': addr_hi,
    'seq_lo': '0', 'seq_hi': '999999999999', 'count': 200,
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
for e in r.get('entries', [])[:200]:
    print(f"  seq={e['seq']} func={e['func']} ra={e['ra']} a0={e['a0']} a1={e.get('a1','?')} v0={e.get('v0','?')} frame={e['frame']}")

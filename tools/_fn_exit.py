"""fn_exit_dump for a function. Args: addr_lo addr_hi"""
import socket, json, sys
addr_lo = sys.argv[1]
addr_hi = sys.argv[2]
s = socket.create_connection(('127.0.0.1', 4370), timeout=120)
s.sendall((json.dumps({
    'id': 1, 'cmd': 'fn_exit_dump',
    'addr_lo': addr_lo, 'addr_hi': addr_hi,
    'seq_lo': '0', 'seq_hi': '999999999999', 'count': 50,
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
for e in r.get('entries', [])[:30]:
    print(f"  seq={e['seq']} entry={e['entry_seq']} func={e['func']} v0={e['v0']} v1={e['v1']} frame={e['frame']}")

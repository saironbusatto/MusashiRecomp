"""Dump card_txn entries by txn_seq range."""
import socket, json, sys
seq_lo = int(sys.argv[1]) if len(sys.argv) > 1 else 0
seq_hi = int(sys.argv[2]) if len(sys.argv) > 2 else 999999
s = socket.create_connection(('127.0.0.1', 4370), timeout=30)
s.sendall((json.dumps({'id':1,'cmd':'card_txn_dump','count':32}) + '\n').encode())
buf = b''
while True:
    c = s.recv(65536)
    if not c: break
    buf += c
    if buf.strip().endswith(b'}'):
        try: json.loads(buf.decode()); break
        except: continue
r = json.loads(buf.decode())
print(f"total_closed={r['total_closed']} open={r['open']}")
for e in r['entries']:
    if not (seq_lo <= e['txn_seq'] <= seq_hi): continue
    tx = ' '.join(b for b in e['tx'][:18])
    rx = ' '.join(b for b in e['rx'][:18])
    print(f"\n  seq={e['txn_seq']:>3} slot={e['slot']} sec={e['sector']} bytes={e['bytes']:>3} end={e['end_reason']:>14} sf={e['start_func'][-6:]} ef={e['end_func'][-6:]}")
    print(f"    TX: {tx}")
    print(f"    RX: {rx}")

"""Dump latest card_txn entries with TX/RX bytes."""
import socket, json, sys
count = int(sys.argv[1]) if len(sys.argv) > 1 else 8
s = socket.create_connection(('127.0.0.1', 4370), timeout=30)
s.sendall((json.dumps({'id':1,'cmd':'card_txn_dump','count':count}) + '\n').encode())
buf = b''
while True:
    c = s.recv(65536)
    if not c: break
    buf += c
    if buf.strip().endswith(b'}'):
        try: json.loads(buf.decode()); break
        except: continue
r = json.loads(buf.decode())
print(f"total_closed={r['total_closed']} open={r['open']} emitted={r['emitted']}")
for e in r['entries']:
    tx = ' '.join(b for b in e['tx'])
    rx = ' '.join(b for b in e['rx'])
    print(f"\n  txn_seq={e['txn_seq']} slot={e['slot']} cmd={e['cmd']} sector={e['sector']} bytes={e['bytes']} acks={e['acks']} end={e['end_reason']}")
    print(f"    start_func={e['start_func']} end_func={e['end_func']}")
    print(f"    TX: {tx}")
    print(f"    RX: {rx}")

"""Find any successful 0x57 (write) txns in the ring."""
import socket, json
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
s.sendall((json.dumps({'id':1,'cmd':'card_txn_dump','count':500}) + '\n').encode())
buf = b''
while True:
    c = s.recv(65536)
    if not c: break
    buf += c
    try: json.loads(buf.decode()); break
    except: continue
r = json.loads(buf.decode())
total = r.get('total_closed', 0)
ents = r.get('entries', [])
print(f"total={total} returned={len(ents)}")
writes = [e for e in ents if e['cmd'] == '0x57']
print(f"0x57 (write) txns: {len(writes)}")
reads_succ = [e for e in ents if e['cmd'] == '0x52' and e['end_reason'] == 'success']
print(f"0x52 (read) success: {len(reads_succ)} (sectors: {sorted(set(int(e['sector'], 16) for e in reads_succ))})")
short = [e for e in ents if e['bytes'] <= 5 and e['cmd'] != '0x52']
print(f"non-read short txns: {len(short)}")
for e in writes[:20]:
    tx = ' '.join(b for b in e['tx'][:18])
    rx = ' '.join(b for b in e['rx'][:18])
    print(f"  seq={e['txn_seq']} slot={e['slot']} sector={e['sector']} bytes={e['bytes']} end={e['end_reason']}")
    print(f"    TX: {tx}")
    print(f"    RX: {rx}")

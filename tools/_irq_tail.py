"""Tail of IRQ-relevant rings: imask_trace, sio_irq, chain_trace, mmio_dump, evcb."""
import socket, json
PORT = 4370
N = 30

s = socket.create_connection(('127.0.0.1', PORT), timeout=30)

def send(p):
    s.sendall((json.dumps(p) + '\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue
    return json.loads(buf.decode())

for cmd in ('imask_trace', 'sio_irq_dump', 'chain_trace',
            'card_txn_dump', 'sio_pc_trace', 'evcb_walk_dump'):
    print(f"\n========== {cmd} ==========")
    r = send({'id':1,'cmd':cmd,'count':N,'tail':1})
    if not r.get('ok', True):
        print("  err:", r)
        continue
    for k in ('total','count','head','tail','newest','oldest','entries'):
        if k in r and k != 'entries':
            print(f"  {k}: {r[k]}")
    es = r.get('entries', [])
    print(f"  entries returned: {len(es)} (showing last {min(N,len(es))}):")
    for e in es[-N:]:
        print(f"    {e}")

s.close()

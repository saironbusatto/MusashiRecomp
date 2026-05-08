"""Diagnose the card chain stall at mc_max_state=12.

Pulls chain_trace + card_txn + sio_irq + sio_pc_trace tails and prints them
sorted by sequence so we can see what byte the chain expects vs receives.
"""
import socket, json, sys
PORT = 4370
N = int(sys.argv[1]) if len(sys.argv) > 1 else 80

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

# 1. card_txn - per-transaction record
print("=" * 70)
print("card_txn_dump (last)")
print("=" * 70)
r = send({'id':1,'cmd':'card_txn_dump','count':N,'tail':1})
print(f"meta: total={r.get('total')} ok={r.get('ok')}")
for e in r.get('entries', [])[-N:]:
    print(f"  {e}")

# 2. chain_trace - per chain-step record
print("\n" + "=" * 70)
print("chain_trace (last)")
print("=" * 70)
r = send({'id':1,'cmd':'chain_trace','count':N,'tail':1})
print(f"meta: total={r.get('total')} ok={r.get('ok')}")
for e in r.get('entries', [])[-N:]:
    print(f"  {e}")

# 3. sio_irq - per SIO IRQ record
print("\n" + "=" * 70)
print("sio_irq_dump (last)")
print("=" * 70)
r = send({'id':1,'cmd':'sio_irq_dump','count':N,'tail':1})
print(f"meta: total={r.get('total')} ok={r.get('ok')}")
for e in r.get('entries', [])[-N:]:
    print(f"  {e}")

# 4. card_data_writes - data buffer writes
print("\n" + "=" * 70)
print("card_data_writes (last)")
print("=" * 70)
r = send({'id':1,'cmd':'card_data_writes','count':N,'tail':1})
print(f"meta: total={r.get('total')} ok={r.get('ok')}")
for e in r.get('entries', [])[-N:]:
    print(f"  {e}")

# 5. card_read_summary - aggregate stats
print("\n" + "=" * 70)
print("card_read_summary")
print("=" * 70)
r = send({'id':1,'cmd':'card_read_summary'})
print(json.dumps(r, indent=2))

s.close()

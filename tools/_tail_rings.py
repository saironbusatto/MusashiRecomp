"""Tail of every event ring — last N entries across all rings, sorted by frame.

Use this when execution is wedged: it pulls the LAST events recorded by each
ring buffer, so you see what was happening right before things stopped.
Never arms anything — purely a query against always-on rings.
"""
import socket, json, sys

PORT = 4370
TAIL = int(sys.argv[1]) if len(sys.argv) > 1 else 40

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

def to_int(v):
    if isinstance(v, str):
        return int(v, 16) if v.startswith('0x') else int(v)
    return v

# 1. fn_entry tail (slice from total-TAIL .. total)
print(f"=== fn_entry tail (last {TAIL}) ===")
r = send({'id':1,'cmd':'fn_stats'})
total = r.get('entry_total', 0)
seq_lo = max(0, total - TAIL)
r = send({'id':1,'cmd':'fn_entry_dump',
          'addr_lo':'0x1FC00000','addr_hi':'0x1FC50000',
          'seq_lo':str(seq_lo),'count':TAIL})
for e in r.get('entries', []):
    print(f"  seq={e['seq']:>6} fr={e['frame']} func={e['func']} ra={e['ra']} "
          f"a0={e['a0']} a1={e.get('a1','?')} a2={e.get('a2','?')}")

# 2. fn_exit tail (returns)
print(f"\n=== fn_exit tail (last {TAIL}) ===")
r = send({'id':1,'cmd':'fn_stats'})
total = r.get('exit_total', 0)
seq_lo = max(0, total - TAIL)
r = send({'id':1,'cmd':'fn_exit_dump',
          'addr_lo':'0x1FC00000','addr_hi':'0x1FC50000',
          'seq_lo':str(seq_lo),'count':TAIL})
for e in r.get('entries', []):
    print(f"  seq={e['seq']:>6} fr={e['frame']} func={e['func']} v0={e.get('v0','?')}")

# 3. wtrace tail
print(f"\n=== wtrace tail (last {TAIL}) ===")
r = send({'id':1,'cmd':'wtrace_stats'})
total = r.get('total', 0)
start = max(0, total - TAIL)
r = send({'id':1,'cmd':'wtrace_dump','start':start,'count':TAIL})
for e in r.get('entries', []):
    print(f"  fr={e.get('frame','?')} addr=0x{to_int(e['addr']):08X} "
          f"old=0x{to_int(e.get('old',0)):08X} new=0x{to_int(e.get('new',0)):08X} "
          f"func=0x{to_int(e.get('func',0)):08X} ra=0x{to_int(e.get('ra',0)):08X}")

# 4. sio_irq tail (per memo: 200k cap)
print(f"\n=== sio_irq_dump tail (last {TAIL}) ===")
r = send({'id':1,'cmd':'sio_irq_dump','count':TAIL,'tail':1})
for e in r.get('entries', [])[-TAIL:]:
    print(f"  {e}")

# 5. chain_trace tail
print(f"\n=== chain_trace tail (last {TAIL}) ===")
r = send({'id':1,'cmd':'chain_trace','count':TAIL,'tail':1})
for e in r.get('entries', [])[-TAIL:]:
    print(f"  {e}")

# 6. card_txn tail
print(f"\n=== card_txn_dump tail (last {TAIL}) ===")
r = send({'id':1,'cmd':'card_txn_dump','count':TAIL,'tail':1})
for e in r.get('entries', [])[-TAIL:]:
    print(f"  {e}")

# 7. imask_trace tail
print(f"\n=== imask_trace tail (last {TAIL}) ===")
r = send({'id':1,'cmd':'imask_trace','count':TAIL,'tail':1})
for e in r.get('entries', [])[-TAIL:]:
    print(f"  {e}")

# 8. mmio_dump tail
print(f"\n=== mmio_dump tail (last {TAIL}) ===")
r = send({'id':1,'cmd':'mmio_dump','count':TAIL,'tail':1})
for e in r.get('entries', [])[-TAIL:]:
    print(f"  {e}")

# 9. sio_pc_trace tail
print(f"\n=== sio_pc_trace tail (last {TAIL}) ===")
r = send({'id':1,'cmd':'sio_pc_trace','count':TAIL,'tail':1})
for e in r.get('entries', [])[-TAIL:]:
    print(f"  {e}")

s.close()

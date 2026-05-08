"""Compact chain diagnostic — header-only, no tx/rx blobs."""
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

print("==== card_txn_dump headers (last 80) ====")
r = send({'id':1,'cmd':'card_txn_dump','count':N,'tail':1})
print(f"meta: total={r.get('total')} len(entries)={len(r.get('entries',[]))}")
for e in r.get('entries', [])[-N:]:
    short = {k: e[k] for k in
             ('txn_seq','slot','cmd','sector','bytes','acks',
              'start_byte_seq','end_byte_seq','start_func','end_func',
              'end_reason','terminal_state','live') if k in e}
    print(f"  {short}")

print("\n==== chain_trace headers (last 80) ====")
r = send({'id':1,'cmd':'chain_trace','count':N,'tail':1})
print(f"meta: total={r.get('total')} len(entries)={len(r.get('entries',[]))}")
for e in r.get('entries', [])[-N:]:
    print(f"  {e}")

print("\n==== sio_irq_dump headers (last 30) ====")
r = send({'id':1,'cmd':'sio_irq_dump','count':30,'tail':1})
print(f"meta: total={r.get('total')} len(entries)={len(r.get('entries',[]))}")
for e in r.get('entries', [])[-30:]:
    print(f"  {e}")

print("\n==== card_data_writes (last 30) ====")
r = send({'id':1,'cmd':'card_data_writes','count':30,'tail':1})
print(f"meta: total={r.get('total')} len(entries)={len(r.get('entries',[]))}")
for e in r.get('entries', [])[-30:]:
    print(f"  {e}")

print("\n==== card_read_summary ====")
r = send({'id':1,'cmd':'card_read_summary'})
print(json.dumps(r, indent=2))

print("\n==== sio_state ====")
r = send({'id':1,'cmd':'sio_state'})
print(json.dumps(r, indent=2))

s.close()

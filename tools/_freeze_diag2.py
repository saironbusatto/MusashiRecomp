"""Diagnose freeze: ping debug server, then dump unknown_dispatch_log
and dispatch_tail to see what's looping/hanging."""
import socket, json, sys
def call(payload, port=4370, timeout=5):
    s = socket.create_connection(('127.0.0.1', port), timeout=timeout)
    s.sendall((json.dumps(dict(payload, id=1))+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        try: return json.loads(buf.decode())
        except: pass
    raise RuntimeError("incomplete")

try:
    print("=== ping ===")
    print(call({'cmd':'ping'}, timeout=2))
except Exception as e:
    print(f"DEBUG SERVER UNRESPONSIVE: {e}")
    sys.exit(1)

# It's responsive — diagnose
print("\n=== unknown_dispatch_log summary ===")
r = call({'cmd':'unknown_dispatch_log'})
print(f"total={r.get('total')} unique={r.get('unique')}")
for s in r.get('summary', [])[:10]:
    print(f"  {s['phys']}: {s['count']:>8}")

print("\n=== unknown_dispatch_log tail (last 20) ===")
r = call({'cmd':'unknown_dispatch_log','tail':20})
for e in r.get('entries', [])[-20:]:
    print(f"  seq={e['seq']:>5} phys={e['phys']} ra={e['ra']} a0={e['a0']} a1={e['a1']} frame={e['frame']}")

print("\n=== dispatch_tail (last 50 dispatched targets) ===")
r = call({'cmd':'dispatch_tail','count':50})
for a in r.get('addrs', [])[-50:]:
    print(f"  {a}")

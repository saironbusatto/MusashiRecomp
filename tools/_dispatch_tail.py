"""See what func_addrs recomp recently dispatched."""
import socket, json
def call(payload):
    s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
    s.sendall((json.dumps(dict(payload, id=1))+'\n').encode())
    buf = b''
    # Read until we have a complete JSON object (ends with } and balanced braces)
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        # Try parsing
        try:
            return json.loads(buf.decode())
        except: pass
    s.close()
    raise RuntimeError("incomplete response")

r = call({'cmd':'dispatch_tail','count':100})
addrs = r.get('addrs', [])
print(f"total dispatches ever: {r.get('total')}")
print(f"last {len(addrs)} dispatch targets:\n")

# Show by address-range bucket
buckets = {}
for a_str in addrs:
    a = int(a_str, 16)
    bucket = a & 0xFFFF0000
    buckets[bucket] = buckets.get(bucket, 0) + 1

print("Bucketed by 64KB region:")
for b in sorted(buckets):
    print(f"  0x{b:08X}: {buckets[b]} hits")

print("\nUnique addresses in last 100:")
for a in sorted(set(addrs)):
    print(f"  {a}")
